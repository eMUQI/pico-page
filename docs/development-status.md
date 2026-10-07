# 开发状态

最后更新：2026-10-07。书库首页直接显示 `Books/` 内容的固件已安装，设备启动和书库首帧经诊断确认；主页实际观感仍待目视确认。书库锁屏后唤醒误入阅读页的修复已通过 host 验证，尚未安装到设备。TF 卡目录已迁移，设备诊断确认新书籍与字体路径可读，原有文章进度可加载。此前界面已按《实现评审 · 修改稿》在分支 `ui-review-20261007` 上调整并安装到设备，手指长按等仍待真机验证。GB18030 TXT 直读固件已安装，用户已确认原先无法打开的书在设备上正常显示中文并可翻页；重启后进度恢复仍待确认。USB 卷名固件已安装，macOS 已显示 `PICO PAGE`；安全弹出后设备退出重启，串口、TF 卡和阅读界面已恢复。
此前阅读响应性专项的设备数据、EPUB 兼容性修复与传书记录见下方；各批次命令、结果及未测项分别记录，不将本轮 host 结果写成设备结果。

## 2026-10-07 · PR #2 评审修正（已验证，待安装）

- 根据 PR #2 的两条评审意见，空书库提示和“存储与设备”页改用实际配置的单一书库扫描根目录。配置多个扫描根时，空书库提示指向已设置的目录，存储页提示在设置文件中查看；文章目录说明不再固定写成 `Books/Articles/`。
- TF 卡迁移说明补充单书 `<原书完整文件名>.picopage/settings.toml` 中的字体覆盖项：旧路径须改为 `Fonts/`，或删除覆盖项以继承全局字体；自定义书库根目录应按实际迁移后的路径填写。
- 新增自定义扫描根的书库首页和存储页画布回归检查。实际执行 `cmake --build build/host -j 8` 通过；`ctest --test-dir build/host --output-on-failure` **11/11** 通过；`python3 tools/check_handoff.py` 通过 338 项，`git diff --check` 通过。`sh tools/build_firmware.sh` 通过，镜像 3,236,224 字节，4 MiB 应用槽剩余 23%。
- 这版固件尚未安装；自定义根目录提示在实体屏幕上的观感待验证。

## 2026-10-07 · 书库锁屏后返回书库（本地实现）

- 设备使用轻睡眠，唤醒后沿用同一个阅读会话。此前 `wake()` 只根据是否打开过书来恢复页面：返回书库后会话仍保留书籍，因此锁屏再唤醒会误入正文。现在锁屏时记录是否应返回书库，唤醒后恢复书库；从正文锁屏仍恢复正文。锁屏提示随返回目标显示“返回书库”或“继续阅读”。
- 使用设备所用的异步内容模式，新增“打开书籍 → 返回书库 → 锁屏 → 唤醒”的回归检查，并确认保存的进度不变。修复前该检查在唤醒后的页面断言失败；修复后 `cmake --build build/host -j 8` 通过，`ctest --test-dir build/host --output-on-failure` **11/11** 通过。
- `sh tools/build_firmware.sh` 首次受沙箱限制，ESP-IDF 组件管理器读取 macOS 进程信息时失败；允许该读取后重新构建通过，应用镜像 3,235,888 字节，4 MiB 应用槽剩余 23%。`python3 tools/check_handoff.py` 通过 338 项，`git diff --check` 通过。尚未安装这版镜像；锁屏画面及唤醒后的真实设备行为待验证。

## 2026-10-07 · 书库首页显示单一根目录内容（已安装，启动诊断通过）

- 单一书库扫描根直接作为“书库”首页，默认情况下直接列出 `Books/` 内的文件夹和 EPUB/TXT，不再单列 `Books` 入口。进入 `Books/Articles/` 等子文件夹后，顶栏返回“书库”；配置多个扫描根时仍列出各根入口。继续阅读、排序、列表/卡片样式保持原结构；目录读取失败显示错误，不当作空书库。
- 684×1216 桌面画布预览：[书库首页](../artifacts/library-home-20261007/home.png)、[Articles 文件夹](../artifacts/library-home-20261007/articles.png)。预览使用合成 EPUB 和本地字体，来自实际 C++ 阅读程序的 `--headless --script` 输出，不是设备照片，也不代表墨水屏效果。
- 调整了依赖旧 `Books` 入口的应用交互测试，并以独立演示卡验证从首页进入子文件夹、返回首页、直接打开根目录书籍。实际执行 `cmake --build build/host -j 8` 通过，`ctest --test-dir build/host --output-on-failure` **11/11** 通过；最后补充目录安全检查后重新构建，`ctest --test-dir build/host -R '^app$' --output-on-failure` **1/1** 通过。最终代码执行 `sh tools/build_firmware.sh` 通过，镜像 3,235,808 字节，4 MiB 应用槽剩余 23%。
- 用户明确授权烧录。镜像固定副本位于 Git 忽略的 `.local/library-home-install-20261007/`，SHA-256 `f1a962a63d487039df8c8187a9673701111607d216b09ee48bfc24f6ce0473ac`。设备 USB 序列号与 `esptool read-mac` 均为 `14:C1:9F:D0:18:B8`。烧录前诊断为 `view=library pending=0 card=1 saved=1`；只读分区表与 OTA 元数据转储和前次安装逐字节一致：`ota_0=0x10000/4 MiB`，sequence 1、state 2 指向 OTA0。只执行 `esptool write-flash 0x10000` 写入应用镜像，报告 `Hash of data verified`，未写 bootloader、分区表、NVS、otadata、OTA1 或 TF 卡。
- 硬件复位后诊断为 `view=library pending=0 card=1 frame_ok=1 clean=1`，首帧已呈现。诊断画布采集返回 `frame_not_confirmed_or_changed`，当时画面正在变化；后续状态为 `view=reading saved=1 frame_ok=1`。尚未由设备画布或目视确认首页的具体书目层级、物理屏幕效果及触摸返回路径。

## 2026-10-07 · TF 卡目录迁移与固件安装（已安装，诊断验证）

- 用户授权烧录。USB 序列号与 `esptool read-mac` 均为 `14:C1:9F:D0:18:B8`；设备复位后诊断为 `view=library pending=0 card=1 importing=0`。经设备 USB 传输页交出 TF 卡，电脑确认 `disk4s1` 为 31.3 GB FAT32 卷 `PICO PAGE`。
- 将根目录 `Articles/` 移至 `Books/Articles/`，将 `PicoPage/fonts/` 移至根目录 `Fonts/`。移动前复制并逐项核对 87 个文件、67,371,639 字节的本地备份；移动后从新路径回读，87 个 SHA-256 均与备份一致。书籍伴随目录随文章目录整体移动；`PicoPage/settings.toml` 未指定旧扫描根或旧字体路径，无须改写。未触碰 `Books/` 现有书籍、阅读进度文件内容、网络/同步凭据或其他 TF 文件。
- 私人备份、逐文件摘要清单和固定镜像保存在 Git 忽略的 `.local/tf-layout-migration-20261007/`。`sh tools/build_firmware.sh` 通过；镜像 3,234,704 字节，SHA-256 `25a59493f6995323b4a7310a73c2b2ac98316f296ce3d14aef2f079e8a964207`，`esptool image-info` 校验镜像类型与内部哈希。`diskutil eject disk4` 成功，电脑已安全弹出 TF 卡。
- 用户在设备上点“退出并重启”后，串口恢复，诊断显示 `view=library pending=0 card=1 importing=0`。烧录前读取并核对分区表及 OTA 元数据：`ota_0=0x10000/4 MiB`，当前有效记录为 sequence 1、state 2，指向 OTA0。使用 `esptool write-flash 0x10000` 只写 3,234,704 字节的应用镜像，报告 `Hash of data verified`，并完成硬件复位；未写 bootloader、分区表、NVS、otadata、OTA1 或校准数据。烧录前的分区表与 OTA 元数据转储保存在上述 Git 忽略目录。
- 重启后诊断显示 `view=library pending=0 card=1 frame_ok=1`。新固件列出 `Books/Articles/` 的 55 个目录项及 `Fonts/` 的 25 个文件，能读取默认字体 `Fonts/LXGWWenKai-Regular.ttf`（24,744,500 字节，CRC32 `e8b2c11f`）。打开迁移前已有进度的一篇文章后进入 `view=reading saved=1 revision=5 frame_ok=1`；修订号与备份进度记录一致。“存储与设备”页进入 `view=storage_settings frame_ok=1 card=1`。这些是设备诊断结果，屏幕实际观感及后续翻页、重启恢复尚未目视确认。

## 2026-10-07 · TF 卡目录与存储页面调整（本地实现）

- 默认书库根目录改为 `Books/`，文章建议放入 `Books/Articles/`；正文与界面字体从卡根 `Fonts/` 读取，配置中的字体路径相对卡根目录。诊断导入只接收 `Books/` 下的 EPUB/TXT 和 `Fonts/` 下的字体/许可文本，不再接收卡根 `Articles/` 或 `PicoPage/images/`。`PicoPage/` 继续管理配置、最近书籍与可重建缓存；单书设置和进度仍在原书旁的 `.picopage/`。
- “存储与设备”页增加书籍、字体、设置和进度的实际路径；空书库提示改为将文件放入 `Books/`。桌面预览截图：[存储与设备](../artifacts/storage-layout-20261007/storage.png)，使用空演示卡，684×1216；相对原稿 S10-3 增加“文件位置”说明，保持原页面结构和操作。`LockScreens/` 只作为未来自定义锁屏图片的目录约定，固件尚不读取，也未增加无效的锁屏选择项。
- 示例配置、字体准备脚本、host 测试字体路径及当前使用说明已更新。完成本地实现时，真实设备的 TF 卡仍有卡根 `Articles/` 与 `PicoPage/fonts/`；本批没有改动该卡或刷写设备。后续迁移与安装结果记录在上方。
- 实际执行：`cmake --build build/host -j 8` 通过；`ctest --test-dir build/host --output-on-failure` **11/11** 通过；最后一处测试夹具路径改动后，`ctest --test-dir build/host -R '^store$' --output-on-failure` **1/1** 通过。`sh tools/build_firmware.sh` 通过，镜像 3,234,704 字节，4 MiB 应用槽剩余 23%；`python3 tools/check_handoff.py` 通过 336 项；`git diff --check` 通过。截图由 host 脚本实际画布导出并核对。首次 host 测试因一条旧字体路径断言失败，修正测试夹具后全套通过。
- 本批尚未做真实 TF 卡迁移和设备验证；后续迁移、烧录与诊断结果记录在上方。字体和存储页面的物理屏幕效果仍待目视确认。

## 2026-10-07 · 继续阅读卡片显示封面（本地实现）

- 对照原始书库设计稿 S01-1，为继续阅读卡片增加 96 × 128 封面；复用书库卡片的缩略图缓存并缩小绘制，不额外生成一套缓存。无有效封面时沿用文字布局；封面生成请求按书籍路径去重。封面出现后，书名与位置移到封面右侧。
- 实现截图：[书库继续阅读](../artifacts/continue-cover-20261007/library.png)。这是合成 EPUB 的桌面预览，原始设计稿没有修改；本次只补封面与对应文字间距，未调整卡片外框、交互或其他页面。
- 实际执行：`cmake --build build/host -j 8` 通过；`ctest --test-dir build/host --output-on-failure` **11/11** 通过，包括新增的继续阅读封面像素检查；桌面脚本从阅读页返回书库后截屏核对；`git diff --check` 通过。`sh tools/build_firmware.sh` 首次在沙箱中因 ESP-IDF 组件管理器调用 `sysctl()` 被拒绝，获准在沙箱外重试后构建通过，应用镜像 3,234,784 字节。
- 本次没有烧录设备；真机封面显示、全刷效果和耗时仍待验证。下一步在设备上用有封面的书打开并返回书库，检查卡片排版与刷新。

## 2026-10-07 · 保存设备书籍与字体作为本地测试样本

- 从已挂载的 `PICO PAGE` 卷只读复制 `Books/` 中的 EPUB、TXT，以及 `PicoPage/fonts/` 中的 TTF、OTF 和两份字体许可文本，到 Git 忽略的 `private/device-test-library/`。后续本机和设备兼容性测试优先使用这批真实样本；公开或可重复的自动化测试仍使用合成或已获许可的样本，不提交私人书籍与字体副本。
- `Articles/` 中 52 个网页导出 EPUB、阅读进度、单书设置、缓存、AppleDouble 文件及未完成的导入临时文件均未复制。按 SHA-256 去重后保留 21 本 EPUB、4 本 TXT、23 个字体；唯一重复的 EPUB 仅删除本地副本，设备原件未改动。`manifest.json` 记录源路径、保留路径、文件大小、摘要和重复对应关系。
- 实际执行：Python 3 逐文件复制时计算 SHA-256，并回读本地副本核对摘要；21 本 EPUB 的 ZIP 成员 CRC 与 OPF 元数据读取通过，4 本 TXT 可用 UTF-8 或 GB18030 解码且不含 NUL，`fc-scan` 成功解析 23 个字体。`git check-ignore -v private/device-test-library/manifest.json` 确认目录被忽略；`python3 tools/check_handoff.py` 通过 332 项，仅覆盖交接资料；`git diff --check` 通过。
- 这是文件可读性检查，不代表每本书或每种字体已在固件、真机屏幕上完成排版与翻页验收。下一步在相关兼容性问题中优先选用这批样本复现，并记录具体测试范围。

## 2026-10-07 · 按《实现评审 · 修改稿》调整界面（已安装，书库页面经诊断口确认）

- 分支 `ui-review-20261007`，基于 `a6af6b8`。此前 main 上未提交的 TXT / GB18030 改动已先单独提交为 `a6af6b8`。没有推送。
- 实现修改稿的 P0 第 1–4 条和 P1 的 01–08，说明、差异和决定见 [UI 注意事项](ui-implementation.md) 第 14 节与 D21–D24。
  P0 第 5 条（正文插图整块黑色）经核实不是缺陷：测试夹具的图片是 2×2 像素的 PNG，放大后面板上方露出的是其左上角的黑色像素。
  P2 没有实现。
- 新增长按输入：`AppInput::long_press`，由 `released_tap.h` 的 `long_press()` 按按下到松手的时长判定，阈值 600 ms。
  桌面预览用鼠标右键，脚本和串口诊断用 `hold X Y`。
- 新增 `PicoPage/cache/positions/`；书库缩略图改为 184×250，旧的 164×184 缓存文件不再使用。格式见 [存储](storage.md)。
- 资源：缩略图内存缓存由 12 张改为 8 张，184,000 字节；书库每屏为每本书多读一个不超过 1 KiB 的位置缓存文件，
  为每个文件夹多做一次目录遍历，结果在离开该目录前保留。目录面板每次重画都要重画正文背景。这些开销都没有在真机上测量。
- 实际执行：`cmake --build build/host -j 8`，`ctest --test-dir build/host --output-on-failure`（**11/11**）；
  `cmake --build build/sanitized -j 8`，`ctest --test-dir build/sanitized --output-on-failure`（**11/11**，无 ASan/UBSan 报错）；
  `sh tools/build_firmware.sh` 通过，日志 `.local/ui-review-firmware.log`，镜像 3,233,584 字节，
  SHA-256 `6cb0c2c4a0b525616fe8c62604d57574bf06470f36c4ddfa0fb3c57a69a4e7a2`，4 MiB 应用槽剩余 23%。
- `tests/app_test.cpp` 按新布局更新了点击坐标，并新增四项检查：长按进入详情、面板内取消同步后留在面板且丢弃迟到的回包、
  提示在下一次输入后清除、离开书后写入位置缓存。`tests/core_test.cpp` 新增长按时长判定。
- 截图：`artifacts/ui-review-impl-20261007/`，六张拼版，来自测试运行时的画布，使用合成 EPUB 和模拟网络回包。
- 初次提交时未验证（当时没有烧录）：长按阈值与误触、封面裁切后的观感、目录面板重画的耗时、书库读取位置缓存的耗时、
  页脚提示与翻屏共用时的可读性，都需要在真机上确认。编译通过和 host 截图不等于真机通过。
- PR eMUQI/pico-page#1 的后续修正（均已推送，host 11/11 通过，未重新构建固件）：
  - CI `host` 在 Linux GCC 下失败的两处原有问题：`image.cpp` 的有符号与无符号比较，`host/main.cpp` 已弃用的 `CURLOPT_PROTOCOLS`（改用 `CURLOPT_PROTOCOLS_STR`，仍只允许 HTTPS）。
  - 位置缓存只在保存确认后写入；换书时尚未保存的位置等保存确认后再写，保存失败不写。新增测试。
  - UTF-8 TXT 的最后一个字符或换行跨过 48 KiB 边界时，不再产生空的末段。新增测试。
  - Wi-Fi 传书页和服务端提示不再写“仅 UTF-8 TXT”，与 GB18030 支持一致。
- Codex 第二轮评审的三条已修正（host 11/11 通过；这三项不在已安装的 `a6764e6` 镜像中）：
  短于 48 KiB 的 TXT 和末段的估算百分比按该段实际字节数计算，不再提前到 100%，新增测试；
  EPUB 打开、定位、摘要和读目录失败归为“文件无法读取”，不再提示内容损坏；
  GB18030 解码表的生成工具版本、可复现性核对和许可情况记入 [依赖审查](dependency-review.md)。
- 用户提出列表视图应有封面：按原稿 S01-1 补上，列表行左侧显示由卡片缩略图缩小的 59 × 80 封面，无封面不留空位，状态移到行右侧。
- 用户授权“可以先烧录”后安装到设备，共写入三次（评审修正版、列表封面版、白封面回退版），每次流程相同：
  确认只连接一台设备，USB 序列号与 esptool `read-mac` 均为 `14:C1:9F:D0:18:B8`；诊断 `status` 为 `view=library pending=0 card=1 importing=0`；
  重新读取分区表和 OTA 元数据，与此前核实过的转储逐字节相同（`ota_0=0x10000/4 MiB`，sequence 1、state 2）；
  只向 `0x10000` 写入应用，esptool 报告 `Hash of data verified`，看门狗复位后书库首帧成功。未写 bootloader、分区表、NVS、otadata、OTA1 或校准数据，未改 TF 卡上的书和进度。
  最终镜像来自提交 `a6764e6`，3,234,176 字节，SHA-256 `184201158ada5ace5cf0be9c80ba8a5f4bfc82333cb36ac445a05a656b7b510a`，应用槽剩余 23%。日志和转储在忽略目录 `.local/ui-review-install/`。
- 真机经诊断口确认：书库首页、Articles 文件夹的列表和卡片视图、`hold` 进入书籍详情均显示正常；画面见 `artifacts/ui-review-impl-20261007/device-library.png`（诊断口导出的应用画布，不是面板照片）。
  卡片视图稳定后 `psram=4859416`、`cache_covers=92000`、`draw_ms=480`。
- 真机发现并已修正：Articles 中多本文章类 EPUB 的封面缩略图解码结果是全白，卡片上没有任何书名。现在全白缩略图按没有封面处理，在卡片上写书名。
  这些封面为什么是全白没有查明（原图可能本来就是白底，也可能是解码结果不对），原书没有读出来检查。
- 真机仍未验证：手指长按的时长判定和误触（`hold` 是应用层注入，不经过触摸采样）、面板实际观感与残影、目录和同步面板、
  打开一本书后书库显示的章节与百分比、翻屏时读取位置缓存的耗时、休眠与重启后的恢复。
- 下一步：在设备上用手指试长按进详情和书库翻屏；查明全白封面的原因。

## 2026-10-07 · PR #1 第三轮评审：混合语言错误提示

- `reason()` 先识别英文诊断中的已知类别；带 `: ` 分隔符的未知详情不再因为资源路径含中文就原样显示。独立的中文错误提示继续保留。
- 新增合成 EPUB：`container.xml` 指向不存在的中文 OPF 路径。应用测试打开它时检查错误页显示中文类别，不显示英文诊断前缀。
- 实际执行：`cmake --build build/host -j 8` 通过；`ctest --test-dir build/host --output-on-failure` 在初版修订后 **11/11** 通过；最终调整错误分类顺序后重新构建 host，并运行 app 单项测试。`sh tools/build_firmware.sh` 对最终代码构建通过，镜像 3,234,368 字节，应用槽剩余 23%。
- 未刷写设备；真机错误页的显示效果仍待设备验证。工作区原有的本地样本记录不随本次提交纳入 PR。

## 2026-10-07 · GB18030 TXT 设备端直读（已安装，已目视确认）

- 只读检查用户提供的原始 1,579,496 字节 TXT：不是有效 UTF-8、GBK 或 Big5，GB18030 可完整解码，且无 NUL；设备端直接读取原文件，不生成转换副本。原书字节和私人路径不纳入仓库。
- UTF-8 优先；无效时尝试 GB18030。使用 Python 标准库编码器生成双字节映射及四字节区间表，固件按流验证原文件，并将 GB18030 段边界对齐到字符起点；在已核实的受控存储会话中复用边界。UTF-8 继续使用 `txt-utf8-v1`，GB18030 使用独立 `txt-gb18030-v1` 阅读与分页修订，不复用另一种编码的记录。其他编码、手动指定编码和 TXT 的 KOReader 接续仍未实现；无标记的旧编码可能误判。
- 实际执行 `cmake --build build/host -j 8` 和 `ctest --test-dir build/host --output-on-failure`（11/11）；修订进度选择后再次运行 document/app 两项（2/2）。合成样本覆盖四字节字符跨 48 KiB 分段及 GBK 不收录的双字节字符。对用户原书逐段读取，共 33 段、794,268 个 Unicode 标量，逐字 FNV 摘要与 Python `gb18030` 解码一致。该原书验证脚本只放在 `/tmp`。
- `sh tools/build_firmware.sh` 首次因沙箱 `sysctl()` 权限失败；获准重试后完整构建通过。安装镜像 3,221,616 字节，SHA-256 `28dfdd4455b2fec72c2091cfaafa82bb127bfe29d633fef46bfa38f698b15b4a`，4 MiB 应用槽剩余约 23%。
- 烧录前设备报告 `view=library pending=0 card=1 importing=0`；串口芯片 MAC 为 `14:C1:9F:D0:18:B8`。重新读取分区表和 OTA 元数据，确认有效的 `ota_0=0x10000/4 MiB`、sequence 1、state 2、CRC 正确。只向 `0x10000` 写入应用镜像，esptool 报告 `Hash of data verified`；未写其他 Flash 分区或 TF 卡。重启后书库首帧成功且 `card=1`。
- 设备 TF 卡上的目标文件与电脑原书长度均为 1,579,496 字节、CRC32 均为 `f136e06f`。设备打开后，诊断从 `view=loading` 进入 `view=reading frame_ok=1`；一次 K3 翻页后画面 CRC 变化，阅读修订号为 1，`saved=1`。抓取完整正文画面时帧发生变化，命令返回 `frame_not_confirmed_or_changed`，因此没有留存完整屏幕截图；用户随后确认中文正常显示并可翻页。重启后进度恢复、其他编码误判和传书入口仍待验证。未提交或推送。

## 2026-10-07 · 一份 TXT 无法打开（已定位）

- 设备诊断 `status` 返回 `card=1`、`frame_ok=1`；使用 `tools/device_console.py open` 复现目标 TXT 打开流程，最终 `view=error`。抓取设备错误画面，提示“TXT 不是有效的 UTF-8 文件，请先转换编码”。文件夹列表显示该文件仍在，大小约 1.58 MB；未读取、修改或删除原文件。
- 当时只定位到 UTF-8 格式边界，尚未取得原文件或确认编码。后续取得原文件并完成 GB18030 设备端直读，见上节；本次故障画面仅存忽略目录 `.local/txt-support/install/`。

## 2026-10-07 · USB 卷名

- 进入 USB 传输、卸载应用侧 FatFs 前，读取 TF 卷标。仅空卷标或 `NONAME` 改为 `PICO PAGE`；保留其他卷标。读取或写入失败时不交出卡。启用 `CONFIG_FATFS_USE_LABEL`，不修改分区或格式。
- 实际执行 `sh tools/build_firmware.sh`：首次在沙箱内因组件管理器的 `sysctl()` 权限失败；获准重试后完整构建通过。安装前重新构建当时的源代码，镜像 3,167,552 字节，SHA-256 `02d1fb4e4c598ada08edc16e192075f78be90e9f7a3321517da06fda05b2658f`；构建日志 `.local/usb-label-build.log`。
- 用户授权“烧录看看”。烧录前诊断 `view=reading pending=0 card=1 saved=1 importing=0`；USB 序列号与 esptool MAC 均为 `14:C1:9F:D0:18:B8`。重新读取设备分区表和 OTA 元数据，确认当前有效 `ota_0=0x10000/4 MiB`、sequence 1、state 2 且 CRC 正确。仅向 `0x10000` 写入上述应用镜像；esptool 报告 `Hash of data verified`。未写 bootloader、分区表、NVS、otadata、OTA1 或工厂校准。
- 看门狗复位后诊断为 `view=library pending=0 card=1 frame_ok=1 clean=1`。诊断触摸进入 USB 传输，macOS `diskutil` 识别 31.3 GB FAT32 分区，卷名为 `PICO PAGE`；`diskutil eject /dev/disk4` 成功，外置磁盘从挂载列表消失。证据见 `.local/usb-label-install/`。
- 用户在设备屏幕退出并重启后，同一 USB 序列号的串口恢复；只读诊断为 `view=tools pending=0 card=1 saved=1 frame_ok=1 importing=0`，说明 TF 卡和阅读界面恢复。未对比重启前后的同一本书位置锚点；证据 `.local/usb-label-install/after-exit-status.log`。本批未推送。

## 2026-10-07 · UTF-8 TXT 阅读支持（已安装，待阅读验收）

- `Books/`、`Articles/` 的文件夹书库现在识别 `.txt`，USB/串口导入与 Wi-Fi 传书接受最大 64 MiB 的 TXT；文件名、同名保护、CRC、同步写入和回读沿用既有存储事务。Wi-Fi 仅检验扩展名和传输完整性，内容编码在开书时验证。
- 文档层按约 48 KiB 的 UTF-8 安全边界读取，支持 BOM、长文本跨段翻页、本地锚点、进度保存与重开恢复。无内嵌目录或封面时使用单个“正文”入口和文字卡片。KOReader 位置接续仍限 EPUB；TXT 同步页明确显示仅本地，不发网络请求。
- 当前只接受有效 UTF-8；GBK/GB18030、UTF-16 与 TXT 章节自动识别未实现。无效 UTF-8 或 NUL 字节在开书时拒绝，保留原文件。未更改已有 EPUB 进度修订格式，TXT 使用独立的 `txt-utf8-v1` 修订。
- 实际执行：`cmake --build build/host -j 8`、`ctest --test-dir build/host --output-on-failure`（**11/11**）、`node tests/wifi_upload_page_test.mjs`、`sh tools/build_firmware.sh`、`python3 tools/check_handoff.py`（328 项）和 `git diff --check` 均通过。首次固件配置在沙箱内被 `sysctl()` 权限拦截，获准重试完成；最终固件 3,164,848 字节，SHA-256 `6d0719de83732a9913a09666cd6357ba887e1730d4a4de5d4a09668a855279e9`。
- 用合成 TXT 的桌面画布检查书库文件名、正文和“仅本地”同步提示；书库原有 EPUB 扩展名截取曾把 `sample.txt` 显示为 `sampl`，已修正并复查。截图在忽略的 `artifacts/txt-support-20261007/`，为 host 画布，字体使用回退路径，不是真机照片。
- 用户授权“烧录”后，仅连接一台 Read Pico；USB 序列号与 esptool 芯片 MAC 均为 `14:C1:9F:D0:18:B8`。烧录前诊断为 `view=library`、`pending=0`、`card=1`、`importing=0`。从设备重新读取分区表和 OTA 元数据，确认当前有效启动槽 `ota_0=0x10000/4 MiB`、sequence 1、state 2 且 CRC 正确。将上述摘要的镜像复制至忽略目录 `.local/txt-support/install/` 后，仅向 `0x10000` 写入 3,164,848 字节；esptool 报告 `Hash of data verified`。未写 bootloader、分区表、NVS、otadata、OTA1 或工厂校准，未擦除 TF 卡。
- esptool 硬复位后串口诊断无应答；按既有设备流程使用 `--after watchdog-reset read-mac` 后成功启动。诊断返回 `view=library`、`card=1`、`frame_ok=1`、`clean=1`，首帧全刷成功。只读分页列出设备 `Books/` 共 52 项，其中 4 个普通 `.txt` 文件仍在。书库根画面可见 Books 文件夹；进入后画面持续变化，抓图返回 `frame_not_confirmed_or_changed`，因此尚未确认 TXT 书目在屏幕上显示。私有证据为 `.local/txt-support/install/` 的分区、OTA、镜像、烧录日志和根画面。
- 未执行：真机 TXT 开书、翻页/回翻、屏幕排版目视、TF 卡读写性能、USB 和手机 Wi-Fi TXT 传输、休眠恢复。未提交或推送。下一步先在设备上用 UTF-8 中文 TXT 验证书目显示、阅读与重启恢复，再评估是否需要 GBK/GB18030 或章节识别。

## 2026-10-07 · 同名重传自动清理未完成临时文件

- 用户确认不应让用户改名绕过残留文件。`FileStore::begin_import` 在正式目标不存在且入口独占卡访问时，
  自动清理该目标的普通 `.import.tmp` 文件，再从头创建事务；不扫描或删除其他文件，不删除目录/符号链接，
  正式 EPUB 仍拒绝覆盖。HTTP 由单服务任务串行处理，串口导入已有活动事务门控，两种入口由主循环互斥。
- 新增显式 `FileImport::cancel`，取消和失败路径检查清理结果；HTTP 预检查只有清理完成才返回“可以上传”，
  清理失败明确提示检查存储卡。析构仍作兜底清理。更新使用说明，不再要求为残留临时文件改名或手工清理。
- 复用现有 `store_test` 覆盖残留后同名完整重传、正式文件拒绝覆盖、非法临时路径保护与取消；
  `store_sync_test` 注入 unlink 失败，验证拒绝开始、错误报告和恢复后可清理。没有新增测试框架。
- host 与 ASan/UBSan CTest 各 **11/11** 通过，网页检查通过；固件完整构建成功，3,158,704 字节，SHA-256
  `92d2deef978f286807ee67f8490ed273ef49d4c8e41cab85d62d094e62cb167c`。
  本批日志在 `.local/wifi-upload/cleanup-*.log`。实体断电残留后的同名上传尚待手机复测。
- 延续安装授权，核对同一设备 USB 身份和烧录前 `saved=1`，仅写 `0x10000`；`Hash of data verified`。
  重启后诊断确认书库首帧成功、`card=1`，证据 `.local/wifi-upload/install/cleanup-{before,flash,after}.log`。
  未提交或推送。

## 2026-10-07 · 手机 Wi-Fi 传书成功（设备日志）

- 修正版 `74bcb1c0…` 的手机复测日志确认：第一次预检查返回 `staging_exists_or_unsafe`，
  明确拒绝上次中断遗留的临时文件；没有覆盖该临时文件。
- 后一次请求接收 **909,849 字节**，设备时间 354,687 ms 开始接收，357,520 ms 进入回读，
  358,157 ms 输出 `Saved`：接收约 **2.833 秒**，提交及回读约 **0.637 秒**，合计 **3.470 秒**。
  `Saved` 仅在 `FileImport::finish` 完成同步、CRC 校验、发布和正式文件回读成功后输出。
- HTTP 任务记录最低剩余栈 **2868 字节**；这次上传和结束期间时间连续，没有出现重启或 panic。
  随后实体触摸 `(390,1140)` 对应“结束传书”，网络任务退出，后续两帧刷新成功。
- 证据：`.local/wifi-upload/install/fix-runtime.log`。仅证明本次手机传输与结束成功；
  新书开读、64 MiB、多文件、满卡/掉电及原书进度恢复尚未验收。最初重启的具体 panic 原因仍未取得。

## 2026-10-07 · 传书重启与上传停滞修正版（已安装，待手机复测）

- 用户报告首次在浏览器“回读校验”时设备退出；诊断确认设备重新启动为书库第 1 帧，未捕获该次 panic，
  因而没有确认具体复位原因。再次上传停在前段时，串口记录设备仍处于热点页；该次不是重启。
- 栈风险：目标编译器 `-fstack-usage` 测得上传处理函数 2304 字节、`FileImport::finish` 208 字节、
  `asset_digest` 4240 字节；三层合计 6752 字节，未计文件系统/驱动，HTTP 总栈仅 8192 字节。
  将回读缓冲改为有分配失败处理的堆缓冲，`asset_digest` 静态栈降至 160 字节；保留回读校验，不增大任务栈。
  这是已验证的栈风险修正，不把它写成已由崩溃日志证明的根因。
- 浏览器新增 `/check` 无文件体预检查：使用同一存储导入检查创建并释放临时事务，15 秒超时。
  无效会话、同名文件或上次中断的临时文件在传整本书前返回错误；保留原文件及残留临时文件。
  接收与提交阶段增加不包含文件名/令牌的日志和任务剩余栈记录，以供复测。
- host 与 ASan/UBSan CTest 各 **11/11**，`node tests/wifi_upload_page_test.mjs` 通过，
  补充预检查拒绝后不发送文件体的断言；本地实际处理函数适配检查覆盖预检查临时文件清理，既有错误路径仍通过。
  `sh tools/build_firmware.sh` 完整构建通过，3,157,456 字节，SHA-256
  `74bcb1c0fbef4e98fd17bf3d41a4e0050cbc6fd21a75956af625cb155d454545`。
- 延续用户授权，重新核对 USB 身份，仅写同一设备 `0x10000`，`Hash of data verified`；采用看门狗重启。
  私有证据 `.local/wifi-upload/install/fix-flash.log`、`fix-runtime.log`，构建与测试日志在 `.local/wifi-upload/`。
- 未确认手机实际完成上传；需刷新网页使用新流程，再核对接收、校验、保存及书库可读结果。未提交或推送。

## 2026-10-07 · Wi-Fi 传书候选已安装（上传待联测）

- 用户授权“烧录看下”。核对串口 USB 身份和 esptool MAC 为既有 Read Pico（末尾 `18:B8`）；
  重新读取分区表及 OTA 元数据：`ota_0=0x10000/4 MiB`，有效 sequence 1、state 2、CRC 正确。
  烧录前阅读记录修订 22，诊断报告 `saved=1`。
- 仅写入 `0x10000` 应用，3,155,168 字节，SHA-256
  `f1389926cca5ee16141a45f215ef205e4d5ecfae79671ed9728bff1172656ebd`，esptool 报告 `Hash of data verified`。
  未写 bootloader、分区表、NVS、OTA 元数据或工厂校准。
- 首次 esptool 硬复位后诊断超时，原始串口没有输出；使用 esptool 看门狗复位后正常启动。
  启动日志确认工厂 VCOM 加载、TF 卡挂载及书库首帧清理全刷成功。
- 经诊断输入进入“设置 → 网络与传输 → Wi-Fi 传书”。热点/HTTP 服务启动后画面完成，
  保持 `view=portal`、`card=1`、`frame_ok=1`；内部空闲约 84 KB，最大连续块 38 KiB。
  页面留在热点会话，供手机实测；热点密码不写入版本记录。
- 网络会话期间诊断模拟 K2 返回 `ERROR busy`，这是诊断接口输入门控，未记为 K2 真机通过。
  实体 K2、手机连接、实际上传、关闭热点与原书进度恢复仍需联测。
- 命令为既有工具链的 `esptool read-flash/write-flash/read-mac` 与 `tools/device_console.py`；
  私有原始证据在 `.local/wifi-upload/install/`。未提交或推送。

## 2026-10-07 · Wi-Fi 传书（候选固件，未安装）

- 新增“设置 → 网络与传输 → Wi-Fi 传书”，复用设备热点、二维码和 HTTP 服务；手机或电脑浏览器可批量选择 EPUB，
  按顺序上传到 `Books/`，单文件最大 64 MiB。沿用既有布局，新增一个按钮；没有修改原始设计。
- 存储使用既有 `FileImport`：每次最多 2048 字节、CRC32 校验、同步与写后回读，拒绝覆盖同名文件。
  会话令牌随热点会话重新生成；上传请求验证令牌、文件名、扩展名和大小。正常取消或失败清理未提交临时文件；
  断电残留不会被自动覆盖。浏览器区分接收进度与保存成功，不把断网或取消后的未知结果标为失败未写入。
- 开启前保存进度并停止后台卡任务，传书期间暂停阅读、同步和自动休眠；结束后等待服务退出并重新扫描书库。
  K2 保持全刷；桌面预览入口同步更新，仅展示热点占位，不提供无线服务。
- 实际验证：`cmake --build build/host -j 8` 与 host CTest **11/11** 通过；`app_test` 补充传书入口、
  K2、进度不变和结束返回书库。`node tests/wifi_upload_page_test.mjs` 验证浏览器 CRC32、中文文件名、
  非 EPUB 拦截和服务端错误反馈。
- `cmake --build build/sanitized -j 8` 与 ASan/UBSan CTest **11/11** 通过，无 sanitizer 错误。
- `.local/wifi-upload/check_handler.py` 提取本次实际 HTTP 处理函数，以 host HTTP 适配和真实 FileStore 执行：
  中文文件名保存、同名拒绝、路径穿越/非法字符、CRC 错误、令牌错误、超时、取消、超限与临时文件清理全部通过。
  这是 host 适配验证，不证明 ESP HTTP 服务或真实 TF 卡行为。
- `sh tools/build_firmware.sh` 完整构建通过，镜像 **3,155,168 字节**，SHA-256
  `f1389926cca5ee16141a45f215ef205e4d5ecfae79671ed9728bff1172656ebd`。
  首次配置受沙箱 `sysctl` 权限限制，经获准的构建重试完成；没有替换工具链或新增第三方依赖。
- 桌面截图已检查，新入口及热点页无裁字；K2 前后 BMP 逐字节一致。截图与使用限制见 [Wi-Fi 传书](wifi-transfer.md)。
  `python3 tools/check_handoff.py`、`git diff --check` 通过。
- 未提交、推送或刷写。尚未完成真实热点连接、手机/电脑上传、满卡/断电、传输中 K2、退出后新书可读与阅读进度保持验收。
  下一步：用户授权安装后按 [Wi-Fi 传书](wifi-transfer.md) 的真机项目联测。

## 阅读响应性专项交接

| 审阅问题 | 当前实现 | 仍需验收的边界 |
|---|---|---|
| 统计和长操作阻塞输入 | 独立内容任务、代次校验、输入取消检查；开书、跨章、图片、封面、详情和位置换算可取消 | 真机取消最长等待；阻塞 SD 调用不能抢断 |
| 后段反复解压、回翻重排 | 规范化内容块保留源映射并缓存已解析前缀；复用匹配版式的分页索引和实际访问历史 | 长章节后段恢复、脚注返回与不同字号的真机对照 |
| 总页数抢占下一页 | 优先准备一页布局/字形/位图，支持下一张图片和相邻分段；统计共用轻量换行算法 | 命中页 P95/P99，以及统计前后的实体翻页延迟 |
| 每次开书强摘要 | 受控未修改存储会话内复用已核验身份；冷开书异步核验 | 换卡、USB、外部修改后的设备核验与恢复 |
| 图片、面板、保存重复重活 | 灰阶图片与正文画布复用；可见封面批次合并；进度异步快照确认 | 大 PNG 实际峰值、写卡失败、低内存面板降级 |
| 缓存与输入一致性 | 可选缓存 4 MiB 目标、分页容器上限、平滑字形回收、溢出计数和接触复位 | 内部 RAM/PSRAM 连续块、各任务栈、溢出后保存/休眠 |
| 缓存身份和同步位置 | 强书籍/字体身份、规范化/分页/渲染版本；多段源锚点及 CREngine 往返对照 | 实体 KOReader 双向接续，不以 HTTP 或桌面对照代替 |
| 显示与功耗 | 保留 K2 清理全刷、既有波形和电源策略；补阶段日志和分位数工具 | 残影、区域刷新收益、不同翻页间隔的能耗与空闲电流 |

本轮没有实现新的格式、波形、区域刷新策略或电源窗口调整。先按[响应性验收矩阵](acceptance.md)
采集真机输入、软件准备、屏幕、保存和资源数据，再决定是否需要这些改动。
`tools/summarize_reader_perf.py` 可以汇总日志，但没有设备样本时不能证明 P95/P99 达标。
首次字体打开、未命中文本页的一页排版/绘制以及显式配置/存储交接仍存在同步工作；
这些边界已记录在[架构实现说明](architecture.md)，不能将本轮改造描述为全系统硬实时保证。

设备已安装本轮固件（提交 `2b98f97`，含 VCOM 门控），见下方安装记录；手机配置功能仍未真机验收。
已有版本 E 的长章节阅读、实体连续翻页及休眠唤醒触摸记录保留在历史条目中，
不作为本轮新调度和缓存代码的验收结果。2026-10-07 已按用户要求推送 `main`。

## 2026-10-07 · 统计任务跨批次保留书籍、字体和分段（已安装，真机对照）

- 目的：消除每批 8 页重新打开 EPUB、重新解析整章和重新打开字体的开销。做法与 CrossMux（`3fb809f`）
  `Section` 的增量构建一致：构建上下文跨多次调用保留解析结果；未复制其代码。
- 新增 `PaginationSession`（`content_job.h`）：持有已打开的 `EpubDocument`、统计专用 `Font`（含字宽缓存）和当前章节分段。
  统计任务通过请求借用它；应用在各批之间保留，仍沿用每批最多 8 页的任务机制和既有取消检查。
  会话成员只由拿到它的任务访问；应用只在没有任务运行时读取或释放。前台和后台各用自己的字体对象。
- 会话结束的条件：计数代次变化（换书、换章加载、重排、位置换算、缓存回收）、停止统计任务（存储交接、卡失效）、
  离开正文视图、启动下一章或图片预取任务。普通翻页只取消在途任务，会话保留。
- 真机第一次运行发现统计卡在 `waiting`（`scenarios-stuck.log`）：会话常驻后，下一章的内存准入检查按“此时不持有任何分段”计算，
  一直不过。修正：需要加载新分段时先释放旧分段再检查，不过则结束会话后延迟重试；分页缓存任务的内存检查和缓存回收
  也先结束会话，下一页预取内存不足时会话让路。这类问题 host 无法复现，host 上这些检查恒为通过。
- 测试：`app_test` 的异步统计用例增加断言，230 页的统计期间 EPUB 打开次数小于 5（实测 2，改前每批一次）；
  新增 `epub_open_count()` 计数。分页边界对照：同一本 691 页的书，会话路径与未改动的同步路径生成的分页缓存
  除文件时间和校验摘要外逐字节相同。
- 实际执行：host 与 ASan/UBSan CTest 各 **11/11** 通过；`sh tools/build_firmware.sh` 通过，3,146,096 字节，
  SHA-256 `fb1cd61372186f67b521a6690d9940c0985160dff664dc9c79a29094d111ce6e`；
  日志 `.local/count-session-{host,sanitized,firmware}.log`。
- 安装（用户授权“刷入并对照”）：先装 `5977d831…`，发现上述卡住后装修正版 `fb1cd613…`；两次均核对 USB 序列号与
  `read-mac` 为 `…:18:B8`，仅写入 `0x10000`，`Hash of data verified`。证据在 `.local/count-session-install/`。
- 真机对照（诊断口，文楷 40 px，《呐喊》460 页）：

  | 场景 | 改前 | 改后 |
  |---|---|---|
  | 不打扰，从开书到出总页数 | 无直接数据；按每批 3.5–5 秒估算至少 200 秒 | 38.2 秒 |
  | 每约 18 秒翻一页 | 465 秒 | 未重测 |
  | 每约 4 秒翻一页 | 不失败，无进展 | 不失败，仍无进展（81 次翻页、424 秒） |
  | 统计任务间隔 | 3.5–5 秒 | 中位数 245 ms |
  | 统计期间 EPUB 打开次数 | 每批一次 | 1 次 |
  | 统计期间 PSRAM 最低空闲 | 约 1.73 MB | 约 2.11 MB |

  统计任务栈最低剩余 10,292 字节（32 KiB 栈），内部空闲 103–137 KB，与改前相同。
  4 秒翻页场景中翻页准备耗时：命中 41 次中位数 67 ms，未命中 42 次中位数 128 ms，与改前记录的 126 ms 未命中相当。
- 4 秒翻页仍无进展的原因：该书第一个分段的首次 XML 解析约 2.7 秒，在两次翻页之间完成不了，每次取消后从头解析
  （424 秒内解析阶段启动 162 次）。会话只能保留已完成的解析结果。停止翻页后约 45 秒内统计完成。
  解决需要可恢复的解析或让解析不被翻页取消，尚未做。
- 其他观察：不打扰场景中解析共约 8.5 秒，占 30 秒统计时间的近三成；约每 2.2 秒出现一次约 1 秒的任务间隔，
  周期与脚本查询状态的周期一致，可能由测量引入，未证实。内容缓存的 4 MiB 准入门槛和统计路径不写内容缓存两项未改。
- 未测：实体翻页、分页缓存在设备上的命中、长时间阅读后的内存碎片、休眠唤醒后的会话状态。

## 2026-10-07 · 内置字体扩为 3,500 常用字（已安装；内置字体正文路径未在真机验证）

- 目的：TF 卡上没有字体时仍能阅读简体中文书。做法参考 CrossMux（`3fb809f`）内嵌约 3,500 个高频字的子集；
  本项目用矢量字体，一份子集用于所有字号。来源与许可见[依赖审查](dependency-review.md)和[字体说明](graphics-runtime.md)。
- `tools/setup_fonts_subset.py` 增加《通用规范汉字表》一级字 3,500 个（`fonts/common_characters.txt`，
  取自 Unicode 17.0.0 Unihan 的 `kTGH` 字段）和正文常见符号范围，并保留 `.notdef` 轮廓。
  `PicoPageFallback.otf` 由 537 个字符、102,940 字节变为 4,084 个字符、916,420 字节，
  SHA-256 `18d9a0d50b473590d09bbce06c60371439984f1d3cff8a890f4bc748db6c19be`；原有 537 个字符全部保留。
- 正文使用内置字体且当前页有缺字时，原先整页拒绝显示并报错，现在照常显示，缺少的字符画成方框，
  状态栏提示“缺字，请复制字体到 PicoPage/fonts”。排版设置在字体回退时仍不能应用，这一点没有改。
- `firmware/partitions.build.csv` 的应用分区由 3M 改为 4M，与设备 `ota_0` 槽同大。这只影响本地编译的尺寸检查，
  没有修改设备分区表。
- 测试：`graphics_test` 增加常用字覆盖和缺字方框检查；`app_test` 增加无字体时开书、显示、保存进度和翻页。
  原有两处“无字体时开书或翻页失败”的断言随行为变更改写；显示失败后保留已保存位置的检查仍由 `complete_frame(false)` 路径覆盖。
- 实际执行：`cmake --build build/host -j 8` 与 host CTest **11/11** 通过；
  `cmake --build build/sanitized -j 8` 与 ASan/UBSan CTest **11/11** 通过；
  `sh tools/build_firmware.sh` 完整构建通过；`python3 tools/check_handoff.py`（320 项）与 `git diff --check` 通过。
  日志为 `.local/builtin-font-{host,sanitized,firmware,handoff}.log`。
  第一次 host 运行 `app` 失败在旧断言 `app.view() == View::Error`，改写后通过。
- 固件 `firmware/build/pico_page.bin`：3,142,720 字节，4 MiB 槽剩余 1,051,584 字节（25%），
  SHA-256 `605095784fae75a9c2bf25df6c264c65dd6455989174cb1b02722b990a82c449`。
  链接映射显示 `kFallbackFont` 位于 `.rodata`（Flash 映射），不占 RAM。
- host 无字体渲染：简体演示书 3,569 个字符缺字 0；繁体《呐喊》95,813 个字符缺字 19.10%，
  《阿Q正传》42,070 个字符缺字 19.34%，缺字显示为方框。内置字体不覆盖繁体。
- 用户授权“刷到设备上试试”后安装。只连接一台设备，USB 序列号与 esptool `read-mac` 均为 `…:18:B8`；
  刷机前 `view=reading saved=1 revision=113 crc32=6a597379`，卡上文楷和 Noto 的长度与 CRC 已记录。
  仅写入 `ota_0`（`0x10000`），3,142,720 字节，23.0 秒，`Hash of data verified`。
  重启 4.6 秒后书库帧成功。诊断口点“继续阅读”恢复到修订 113，画布 CRC 仍为 `6a597379`；
  K3 后修订 114、CRC `cf5567bb`，K1 返回后修订 115、CRC 恢复为 `6a597379`，各帧 `frame_ok=1`。
  证据在 `.local/builtin-font-install/`：`pre-status.log`、`premac.log`、`flash.log`、`post-status.log`、
  `resume-status.log`、`k3-status.log`、`k1-status.log`。
- 以上只验证卡上有完整字体时启动、恢复和翻页没有回归，属于诊断注入和画布摘要，不是面板实拍。
  设备卡上有文楷和 Noto，内置字体的正文路径没有触发。未测：真机上 FreeType 打开 916 KB CFF 字体的堆占用和耗时、
  内置字体下的翻页延迟、面板上方框和小字号的显示效果。验证方法是在 USB 传输模式下把 `PicoPage/fonts` 临时改名后重启开书。
- 真机无字体验证（用户在 USB 传输模式下授权，把卡上 `PicoPage/fonts` 改名为 `fonts-off` 后重启）：
  简体书正文用内置字体正常显示；繁体《呐喊》首页“吶”显示为方框，状态栏出现缺字提示。
  阅读时内部空闲 137,303 字节、最大内部块 90,112 字节，与有字体时相同；总空闲约 4.55 MB，最低 4.25 MB；
  面板绘制 483–484 ms，与文楷相同；40 px 下 437 个字形的位图缓存为 592,622 字节。
  《呐喊》后台统计页数持续推进（页索引 700 → 2,260 字节），观察约 2 分钟内未完成。
  画布导出为 `.local/builtin-font-install/nofont-page1.pgm`、`nofont-nahan.pgm`，状态为 `nofont-*-status.log`。
  这些是诊断口读数和画布导出，不是面板实拍。字体打开耗时没有单独测量。
- 真机发现并修复：字体提示一旦出现就一直占用状态栏，页码不再显示。现在缺字提示只在有缺字的页面显示；
  开书时的字体回退提示在第一次翻页后让位给页码。无任何字体时开书提示改为“未找到正文字体，已使用内置字体”。
  `app_test` 增加对应断言，并增加内置字体下后台页数统计能完成的检查。
  修复后 host 与 ASan/UBSan CTest 各 **11/11** 通过，固件构建通过：3,143,120 字节，
  SHA-256 `877756721fad1f37adb743a58db808cd2fdfd58e063711c92c3842f8190f1f9d`。
- 用户在 USB 传输模式下授权后，卡上目录已改回 `PicoPage/fonts`（25 个文件；重启后文楷和 Noto 的长度与 CRC 和改名前一致）。
  用户授权“刷修复版”后安装 `87775672…`：USB 序列号与 `read-mac` 均为 `…:18:B8`，仅写入 `0x10000`，
  3,143,120 字节，`Hash of data verified`，重启后书库帧成功。证据为 `.local/builtin-font-install/fix-*.log`。
  状态栏修复在真机上的显示尚未由诊断口或实体操作确认。
- 页数统计 `failed` 的原因已查明，与字体无关：统计任务每批都重新打开 EPUB，输入取消正在打开的任务时，
  `open_verified` 返回“EPUB open cancelled”，被当作统计失败，本次阅读会话不再统计。用户重启后连续翻页正好触发。
  host 探针对文楷和内置字体都能复现；同一本书在设备上不受打扰时用文楷可以持续统计。
  修复：统计任务被取消时不上报打开错误（`content_job.cpp`），下一批任务重试。
  `app_test` 增加“统计任务被反复取消后不进入 failed 并最终完成”的检查；修复前探针为 `failed`，修复后连续取消 120 秒仍为 `counting`。
  host 与 ASan/UBSan CTest 各 **11/11** 通过，固件构建通过：3,143,136 字节，
  SHA-256 `00c6693e119faa0f15346241eb129f56df4ddaffe4f5dc7449ebf0bf88443e26`。
- 设备日志确认了同一原因：安装前的 `87775672…` 版本上，一次触摸落在统计任务的 `epub_open` 阶段，约 4 秒后状态变为 `failed`
  （`.local/builtin-font-install/count-raw.log`）。用户反馈此前没有任何一本书统计出过页数。
- 用户授权后安装 `00c6693e…`：USB 序列号与 `read-mac` 均为 `…:18:B8`，仅写入 `0x10000`，3,143,136 字节，`Hash of data verified`。
  真机验证（诊断口注入按键，文楷 40 px，《呐喊》）：每约 4 秒翻一页共 19 次，状态一直是 `counting`，但页索引没有增长，
  因为每批统计都在完成前被取消（`count-fix-verify-4s.log`）；改为每约 18 秒翻一页，26 次翻页、465 秒后
  `pages=460 count=idle`（`count-fix-verify.log`）。这是诊断注入，不是实体操作；状态栏页码的面板显示未确认。
- 统计速度的实测构成（`count-raw.log`，另一本书）：每批 8 页约 3.5–5 秒，其中重新打开 EPUB 约 0.32 秒、
  重新解析整章 XML 约 1.0 秒，其余约 1.9 秒是重新打开字体和 8 页排版。每批任务都新建文档、章节和字体对象，
  字宽缓存每批清空。这是统计慢的主要原因，尚未修改；连续快速翻页时统计没有进展也由此而来。
- 调试时把用户卡上的一本 EPUB 临时复制到了会话暂存目录用于 host 复现，没有放进仓库。本批已按用户要求提交并推送 `main`。

## 2026-10-07 · 全书页数统计阻塞修复（已安装，已确认设备分页推进）

- 修复字体摘要被输入反复取消后从头重算：摘要读取游标与 SHA-256 状态保留在本次字体会话，
  每个后台任务最多读取 64 KiB；与每批最多 8 页的分页任务交替执行。分页不再等待摘要完成。
  换字体、存储失效或存储交接时释放扫描对象；迟到的旧字体结果不接入新字体会话。
- 强字体摘要仍是持久分页缓存的校验条件，未降低缓存身份验证。晚到的缓存与当前已完成章节合并，
  不清空本次统计结果；合并受既有 1 MiB 页索引预算约束。
- 分页缓存准入由固定 4 MiB 空闲/2 MiB 连续块改为编码载荷与展开索引的上限预算：
  空闲至少 2 MiB + 256 KiB，连续块至少 1 MiB + 64 KiB。准入或任务启动失败时跳过本会话缓存，
  不再阻止实际分页；同步模式缓存写入失败也结束本次可选写入，不进入空转。
  这些门槛来自容器上界，尚未完成真机峰值测量。
- 统计内存暂时不足时延迟 1 秒重试，不再锁到下一章。状态栏区分“等待统计页数”，
  诊断接口增加 `font-identity`；状态文字使用现有字体子集，未新增字形。
- 在既有 document/app 测试中补回归：多次暂停后摘要仍与整文件摘要一致；为测试字体追加 8 MiB 无关尾部，
  页数在摘要完成前可用；将分页缓存目录阻断为普通文件后仍完成统计，重开及同步参考路径总页数一致。
  既有检查继续覆盖旧会话隔离、强身份缓存失效、阅读位置和画布不被后台工作更改。
- 实际执行：`cmake --build build/host -j 8` 与 host CTest **11/11** 通过；
  `cmake --build build/sanitized -j 8` 与 ASan/UBSan CTest **11/11** 通过；
  `sh tools/build_firmware.sh` 完整构建通过；`python3 tools/check_handoff.py` 与 `git diff --check` 通过。
  日志为 `.local/page-count-fix-{host,sanitized,firmware,handoff}.log`。
  初次固件构建被沙箱 `sysctl()` 权限阻止，授权后使用原 ESP-IDF 环境构建成功；
  中间回归发现同步模式错误等待字体摘要的循环条件，已修正并重新通过全部检查。
- 最终本地应用 `firmware/build/pico_page.bin`：2,329,360 字节，
  SHA-256 `95f47a1a4fcfd30519bca4d710b5664430dfbde18166db8d5c5affb90326dc66`。
- 用户授权“刷机”后，核对唯一连接设备的 USB 序列号与芯片 MAC 均为 `…:18:B8`，
  刷机前 `pending=0 saved=1 revision=113`。仅写入 `ota_0`（`0x10000`），`Hash of data verified`，重启后书库帧成功。
  通过“继续阅读”恢复后，修订号仍为 113，正文 CRC 仍为 `6a597379`，与刷机前一致；
  后台已开始分页，约 94 秒的状态采样中页索引容量由 700 增至 5,256 字节；
  末次采样总页数仍未完成，不将索引增长等同于全书页数验收通过。
  证据为 `.local/page-count-install/` 中的 `pre-status.log`、`premac.log`、`flash.log`、`post-status.log` 、`counting.log` 和 `counting-followup.log`。
- 已随内置字体一批提交并推送 `main`。连续实体翻页、换字号、重开缓存与低内存时的输入延迟仍待专项验收。

## 2026-10-07 · 文字页位图预取改在主任务绘制（已安装，实体操作验证命中）

- 依据第一轮真机数据：后台位图任务要求 PSRAM 空闲 ≥ 3 MiB，阅读稳态只有 2.7–3.05 MB，任务从不运行；
  即使运行，任务自带的新字体没有字形缓存，耗时 200–978 ms，慢于主任务直接绘制。
- 修改（未提交）：`prepare_next()` 在字形预热完成后，用主任务的正文字体和字形缓存把下一页画进独立画布；
  准入沿用正文画布缓存的条件（空闲 > 2 MiB 且最大连续块 > 416 KiB）。输入到达时放弃本次绘制，下一步重做。
  删除 `ContentJob::Kind::RasterPage`、`raster_page()` 和请求里的页面布局副本；下一张图片和相邻分段的预取仍在内容任务。
  新阶段名 `next_page_bitmap`。内容任务结束日志增加 `kind`、`psram_free`、`psram_min`，用于按实测重定其余准入门槛。
- 实际执行：host 与 ASan/UBSan CTest 各 11/11（含位图命中与参考画面逐像素一致的既有回归）；`sh tools/build_firmware.sh` 成功，
  产物 `.local/responsiveness-20261007/pico_page-bitmap.bin`，2,327,056 字节，
  SHA-256 `1fc070d01c3c94ead0d746fd78d81c8979e12b5e864145d8c209b831acd5fa4a`。
- 安装：用户确认（“刷吧”）后写入，仅 `0x10000`，`Hash of data verified`；写入前设备无待处理帧、进度已保存（修订号 14），
  USB 序列号与芯片 MAC 均为 `…:18:B8`（`pre-status-bitmap.log`、`premac-bitmap.log`、`flash-bitmap.log`）。重启后进入书库。
- 真机数据（用户实体操作约 3 分钟，98 次正文翻页，`reader-run-bitmap.log`，状态查询间隔 30 秒；单次小样本）：
  - 上一帧完成 1 秒后的翻页 39 次，全部命中。`body_prepare_hit` 共 42 条：P50 49.5 ms，P95 67.7 ms，最长 80 ms。
    P95 ≤ 50 ms 的目标未达到，P99 ≤ 100 ms 达到。第一轮同类翻页（仅字形预热）为 105–131 ms。
  - 上一帧完成 1 秒内的翻页 59 次，命中 3 次；未命中 56 次：P50 183 ms，P95 284 ms，最长 864 ms。
  - `next_page_bitmap` 45 条，P50 74 ms，最长 78 ms，在主任务执行；同期 `input_wait/unqueued` 84 条，P95 6 ms，最长 12 ms。
  - 资源：持有位图时 PSRAM 空闲最低 2,868,052 字节，最大连续块最低 2,424,832；内部 RAM 最低 104,043；
    内容任务最低剩余栈 9,716。`cache_evictions=0`，输入溢出 0，没有警告或错误日志。
  - 面板：局部刷新绘制 P50 484 ms、复制 77 ms，占一次命中翻页总时间的九成左右。
- 更正：整书页数一直为 0 的直接原因不是下面列出的内存门槛。日志中 `kind=1`（字体身份）任务在约 3 分钟内结束 77 次：
  它对 24.7 MB 的正文字体文件做完整摘要，每次输入都会取消并从头重来，阅读期间从未完成，统计因此没有开始。尚未修复。
- 同一批数据中尚未处理的问题：
  - 字形缓存上限 1 MiB 只容纳约 620–800 个字形（每个约 1.3–1.7 KB），阅读中一直顶在上限。中文正文每页都有一批字形要重新栅格化，
    这是未命中时 `page_draw` P50 195 ms 的主要来源；预热完成后的翻页为 105–131 ms。快速连翻时预取来不及，只有提高缓存命中才有效。
  - 后台工作在最后一次输入 1 秒后才开始（`idle_background_us`），进度提交在 2 秒后；上一帧完成后 1 秒内的翻页不可能命中。本批未改。
  - 其余准入门槛同样高于稳态内存：图片预取 5 MiB、相邻分段预取 5 MiB 且连续 2 MiB、分页索引读写 4 MiB 且连续 2 MiB，
    阅读一分钟后均不满足；前台图片 3 MiB 且连续 2 MiB、书库封面 3 MiB 处于临界。
    这些任务的实际峰值未测，本批只加了日志，没有调整门槛。
- 未验证：持有位图时打开图片页的准入（本轮没有图片请求）；长时间阅读；功耗。

## 2026-10-07 · EPUB 兼容性三项修复已安装（诊断按键验证，待用户实体确认）

- 范围（均在 `document.cpp`，未提交）：
  1. calibre 封面页 `preserveAspectRatio="none"`（见下一条）。
  2. 正文为空而 `body` 的内联样式带 `background-image:url(…)` 的页面，把该图片作为一页图片显示，源锚点为 `body` 节点；
     正文不为空时不添加。只读取内联样式，样式表里设置的背景图不处理；真正空白的章节仍报
     `Chapter contains no supported body content`。
  3. XHTML 1.x DTD 的命名字符实体（`&nbsp;`、`&mdash;` 等 Latin-1 全集和常用符号）按一个字符并入所在文本节点；
     未列出的实体报 `Unsupported XML entity: <名称>`。此前任何命名实体都会使整章失败。
     没有外部 DTD 声明的文档（如 `<!DOCTYPE html>`）里的命名实体仍是 XML 格式错误，未处理。
- 起因：《乡土中国》修好空书名页后，host 探针继续报 `Unresolved XML entities are unsupported`；
  该书 22 个 spine 文件中 19 个含 `&nbsp;`，修复前除封面外基本无法阅读。
- 新增夹具 `background-page`、`background-text`、`named-entities`、`unknown-entity` 及断言
  （实体与相邻文字属于同一个 run，偏移连续）。
- 实际执行：host 与 ASan/UBSan CTest 各 11/11；`sh tools/check_public_books.sh` 对《乡土中国》《素食者》《看见孩子》
  和公开的《呐喊》三种字号全书通过，缺字 0，《乡土中国》书名页图片输出。`sh tools/build_firmware.sh` 成功，
  产物 `.local/responsiveness-20261007/pico_page-parser.bin`，2,330,368 字节，
  SHA-256 `bde9d67312bf819f0ba72c8ea0b9b418ad2c591d1978470c1e394f657716af20`。
- 安装：用户确认（“同意，一起做完再刷”）后写入，仅 `0x10000`，`Hash of data verified`；写入前设备无待处理帧、进度已保存，
  USB 序列号与芯片 MAC 均为 `…:18:B8`（`pre-status-parser.log`、`premac-parser.log`、`flash-parser.log`）。重启后进入书库。
- 真机验证（`verify_books.py`，诊断串口 `open` 与 `key`，日志 `verify-books.log`）：三本书各自打开后 K3×3、K1×3，
  全程 `view=reading`、`frame_ok=1`，每次 K3 画布 CRC 变化，K1 返回后 CRC 与去程逐页一致；
  没有 `Error shown` 或 `Image unavailable` 日志；《乡土中国》书名页和《素食者》封面有解码记录。
  这是诊断注入和画布摘要，不是面板实拍或实体输入。
- 副作用：验证后设备停在《看见孩子》的原位置（修订号 8 未变）；《素食者》在切书时新写了一条位于首页的进度记录。
  验证期间修订号不随翻页增加，是因为串口请求会重置 2 秒空闲保存计时（`app_main.cpp` 的 `idle_save_us`），切书时已提交。
- 未验证：KOReader 对含 `&nbsp;` 的文本和 `body` 锚点的位置换算（未运行 CREngine 对照）；用户实体操作下的显示效果；
  `&nbsp;` 在行首和行尾的排版观感。

## 2026-10-07 · calibre 封面页图片无法显示：已修复（host 验证，未安装）

- 真机日志 `.local/responsiveness-20261007/reader-run-errlog.log`：用户实际操作只有最后约 27 秒落在采集窗口内。
  《乡土中国》封面页图片正常显示；翻到下一页记录到 `Error shown: 章节读取失败：Chapter contains no supported body content`，
  与 host 复现一致。本次没有出现 `Image unavailable`。空闲期间有一次 `PMU event: ESP_ERR_INVALID_RESPONSE`（09:15:45），
  随后状态查询正常，原因未查。
- 图片失败的根因（host 复现）：按第一轮日志中的身份摘要耗时推断出两本小书，文件大小与卡上一致（233,728 和 266,940 字节），
  用户本机副本经 `sh tools/check_public_books.sh` 均报 `Image 0:0: Invalid internal image path`。
  两本都是 calibre 生成的 `titlepage.xhtml`：`<svg … preserveAspectRatio="none"><image … xlink:href="cover.jpeg"/></svg>`。
  解析器只接受缺省值和 `xMidYMid meet`，其余写法会清空图片资源路径，图片页因此显示“图片无法显示”。
  书库封面取自 OPF 声明的封面文件，不经过这条路径，所以能显示。
- 修改（未提交）：`document.cpp` 接受 `preserveAspectRatio="none"`，图片仍按自身比例适配绘制，不拉伸；
  解析结果变化，章节内容缓存版本升为 `content-reading-v2`，旧缓存不再读取（卡上的 v1 文件未清理，可重建）。
  新增 `calibre-svg-cover.epub` 夹具和断言。
- 实际执行：host CTest 11/11；两本书的探针全书通过并输出封面图。sanitizer 与固件构建未重跑，未安装。
- 未修：《乡土中国》的空正文书名页（`body` 背景图）仍报错，处理方式待定。
  错误页上的三次点击没有产生新画面（`prepare=0`、无帧），是否为控件之外的点击未确认。

## 2026-10-07 · 错误原因写入串口日志；《乡土中国》章节失败原因（已安装，待复现）

- 起因：用户报告书内图片显示“图片无法显示”，以及《乡土中国》有章节加载失败。第一轮日志没有任何错误文本：
  图片失败只画统一提示，`error()` 显示的错误也不写串口。
- 修改（未提交）：`app.cpp` 在 `error()` 入口记录 `Error shown: …`，图片任务失败时记录
  `Image unavailable: resource=… error=…`，均为 `reader_app` 的警告级日志，仅固件构建生效。没有改变任何行为。
- 第一轮日志对图片失败的限定：《瓦尔登湖》的 5 次图片请求都完成解码并显示；失败的两次都是开书后的第一页，
  图片任务在 `epub_open` 结束后 2–3 ms 内返回，没有解码记录，当时内存准入已通过。
  因此失败发生在查找图片或识别格式阶段，具体错误待新日志确认；不是内存不足，也不是解码超时。
- 《乡土中国》已在 host 用生产解析器复现（`sh tools/check_public_books.sh`，用户本机的个人 EPUB，未纳入仓库）：
  `Chapter 1: Chapter contains no supported body content`。第二个 spine 项 `Text/front001.html`（书名页）的
  `<body>` 没有任何子节点，图片只写在 `body` 的 `style="background-image:url('../Images/imageSMY.jpg')"` 里。
  解析器把没有正文的章节判为错误（`document.cpp` 的 `load`），应用因此显示章节读取失败。尚未修复。
- 实际执行：host CTest 11/11；`sh tools/build_firmware.sh` 成功。产物 `.local/responsiveness-20261007/pico_page-errlog.bin`，
  2,327,280 字节，SHA-256 `16452f061405aa980bd27933a124eab0c62ac876f85d1dc00022ccc3333f1b09`。
  用户确认（“加错误日志，重新刷机吧”）后写入，仅 `0x10000`，`Hash of data verified`；写入前设备在书库、进度已保存，
  USB 序列号与芯片 MAC 均为 `…:18:B8`（`pre-status-errlog.log`、`premac-errlog.log`、`flash-errlog.log`）。
  重启后进入书库，TF 已挂载，首帧成功，内部 RAM 137,643 字节（`post-status-errlog.log`）。
- 未验证：新日志尚未在真机上捕获到任何错误；sanitizer 未重跑。
- 下一步：用户复现图片失败后读取 `reader-run-errlog.log`；决定空正文章节的处理方式后再修。

## 2026-10-07 · 响应性专项第一轮真机数据（实体操作，约 4 分钟）

- 采集：用户在设备上实际操作（开三本书、连续翻页、改字号、目录、图片页、返回书库），串口日志
  `.local/responsiveness-20261007/reader-run.log`，每 10 秒附带一次 `status`；
  `python3 tools/summarize_reader_perf.py` 汇总。没有注入输入。以下为单次小样本，尾分位数仅供排查。
- 位图预取未生效：`body_prepare_hit` 0 条，`bitmap_hits=0`，`body_prepare_miss` 101 条
  （P50 247 ms，P95 557 ms，最长 1021 ms）。“命中页 P95 ≤ 50 ms”没有样本，不能验收。
  - `ContentJob::raster_page()` 要求 PSRAM 空闲 ≥ 3 MiB。正文字形缓存到约 1 MiB 后，阅读时空闲稳定在
    2.7–3.05 MB，任务立即返回内存不足并关闭本页位图预取。8 次完成的位图全部在开书后 40 秒内（空闲 > 3.1 MB）。
  - 位图任务每次新开字体，不共享主任务字形缓存，耗时 200–978 ms（P50 408 ms），慢于主任务直接绘制
    （`page_draw` P50 195 ms）。预取还要等面板刷新（约 560 ms）和进度提交（P50 239 ms）结束后才开始。
  - 本轮 101 次翻页中 61 次在上一帧完成后 300 ms 内到达，89 次在 1 秒内；这种节奏下即使准入通过也来不及命中。
- 达标项：`input_wait/unqueued` 108 条，P95 3 ms，P99 14 ms，最长 22 ms（目标 P95 ≤ 30、P99 ≤ 100）。
  输入队列溢出 0，没有警告或错误日志，没有重启。
- 取消检查间隔 `zip/checkpoint_max` 68 条：P50 13.7 ms，P95 39.3 ms，最长 52.7 ms，尾部超过 10–20 ms 的目标。
- 图片页：每次图片任务重新打开 EPUB（`epub_open` 约 520–565 ms），再解码 190–625 ms。
  冷开大书的身份摘要 3.79 s、`epub_open` 4.29 s 各一次，在内容任务中执行。
- 资源最小观测值：内部 RAM 95,967 字节（最大连续块 49,152，一个内容任务运行时）；PSRAM 2,706,052 字节
  （最大连续块 1,638,400）；内容任务最低剩余栈 9,732 字节，主任务 11,136，触摸任务 2,288。
  `cache_total` 最高约 1.78 MB，`cache_evictions=0`，4 MiB 回收目标未触发。
- 面板：局部刷新绘制 P50 483 ms，清理全刷约 1522 ms，复制 75 ms；与安装前的 482–483 ms 相比看不出 VCOM 门控带来的耗时变化。
- 整个采集期间 `pages=0`，整书页数没有算完；频繁输入时统计反复被取消。
- 未验证：第一本书打开后 `revision` 从 0 开始，安装前设备停在修订号 10 的那本书是否恢复到原页，
  需要用户确认是否为同一本；VCOM 门控的显示效果；慢速阅读下的命中率；功耗；KOReader 双向接续。
- 下一步：先修正位图预取的内存准入与字形复用，再重测；修正前不继续增加缓存或预取种类。

## 2026-10-07 · 响应性专项固件已安装（仅启动确认，未采集时延）

- 用户确认（“可以，你刷吧”）后，在提交 `2b98f97` 上运行 `sh tools/build_firmware.sh`，构建成功，
  脚本输出 `Read Pico BSP: 5f3406d…`。产物 `.local/responsiveness-20261007/pico_page.bin`，2,327,184 字节，
  SHA-256 `77fe4836e25c238d314badc28e991b5617198cfcd41e9af2ca08d673d82de99c`，ELF 同目录保存。
- 写入前核对：只连接一台设备，USB 序列号与 esptool `read-mac` 均为 `…:18:B8`；设备在正文，无待处理帧，
  进度已保存（修订号 10，第 4 / 409 页，`pre-status.log`）。
- 写入命令与此前相同，仅 `0x10000`，`Hash of data verified`（`premac.log`、`flash.log`）。
  没有写 bootloader、分区表、NVS 或 otadata，没有擦除 Flash 或 TF。
- 重启约 12 秒后读取状态（`post-status.log`）：书库，TF 已挂载，首帧成功且为清理全刷，`draw_ms=1520`；
  内部 RAM 可用 137,643 字节，最大连续块 90,112 字节，与安装前相同；PSRAM 可用 4,993,912 字节；
  主任务最低剩余栈 11,408 字节，触摸任务最低剩余栈 2,288 字节，输入溢出 0，`cache_evictions=0`。
- 没有注入任何输入，没有打开书籍。未验证：验收矩阵中的全部时延分位数、内容任务栈与并发时的内部 RAM、
  取消最长等待、缓存回收、VCOM 门控的显示效果，以及打开书后进度是否恢复到修订号 10。

## 2026-10-07 · 阅读响应性专项：图片预览与面板绘制边界

- 图片页的排版预览接入可取消图像任务；完成后才绘制预览，取消返回工具，已保存位置与排版不变。
  相同图片和目标尺寸复用灰阶缓存，改变字号不重复解码图片。
- 异步模式下 UI 图片绘制不再自行启动解码。正文与磁盘缓存均不可用时，面板采用原有图片不可用状态，
  回到正文仍通过图像任务恢复；不在面板绘制期间隐藏阻塞。同步面板读取内存配置，进入和执行操作时更新/核验账户。
- 内容块序列化及落盘前补取消检查；后台统计也优先读取已有内容块。未命中时沿用轻量解析，不生成缺少源锚点的正文缓存。
- host、ASan/UBSan CTest 各 11/11，固件、交接和差异检查通过。回归包含预览取消、K2、同尺寸重用不解码、
  返回正文后阅读记录不变。日志 `.local/preview-worker-*.log`。未刷机。

## 2026-10-07 · 阅读响应性专项：可取消的同步位置换算

- KOReader 上传前的源位置换算、下载后的候选位置验证改由内容任务执行，独占解析器；加载反馈支持取消和 K2 全刷。
- 返回结果需匹配当前任务代次及同步账户；上传只使用请求捕获且仍然有效的已保存快照。取消换算不发送上传，
  远端映射结果仅形成候选，不推进正文位置或保存修订。
- host、ASan/UBSan CTest 各 11/11，固件构建、交接与差异检查通过；新增回归覆盖换算取消、重试、
  完成后才产生 PUT 请求及阅读记录不变。日志 `.local/mapping-worker-*.log`。未进行网络或真机同步验收。

## 2026-10-07 · 阅读响应性专项：休眠封面不阻塞进入休眠

- 异步详情与切书保存等待已提交 `5cc7ef8`。休眠封面在阅读预取和统计之后低优先级准备，使用既有尺寸/裁剪缓存。
  固件进入休眠时只读取已有封面像素；尚未准备好时采用原有书名画面，不在电源操作路径首次解码大图。
- 受控存储会话中的内置替代字体也复用已打开的字体实例，避免每次绘制重新打开并清空预取。
  存储交接和身份失效仍会使下一次重新检查字体。
- 可选图片预取保留更大的 PSRAM 余量；图片输入分配前检查连续块与保留空间，不把 2 MiB 文件上限当成必然可分配。
- host、ASan/UBSan CTest 各 11/11，固件、交接及差异检查通过。回归覆盖冷进入休眠不解码、后台准备后命中封面
  像素且不重复解码；日志 `.local/sleep-cover-*.log`。未刷机；没有修改睡眠/唤醒来源、波形或面板保持时间。

## 2026-10-07 · 阅读响应性专项：详情核验与切书保存等待

- 分阶段计时已提交 `fe05c0b`。详情页的整书强身份核验、进度读取和封面准备使用可取消内容任务；
  成功核验的身份纳入同一受控存储会话，随后打开这本书无需再次顺序计算摘要。
- 异步切书先呈现加载状态，等待旧书快照提交完成后才启动新书解析。旧进度保存失败会阻止切书，
  保留原已显示/已保存状态；不再在加载反馈之前同步等待写卡。
- 详情渲染只使用已返回的元数据和封面；任务期间 K2 保留全刷，取消返回书库。封面内存不足时采用现有无封面布局，
  不把暂时分配失败写成永久无封面缓存。
- host、ASan/UBSan CTest 各 11/11；补详情取消检查后两套 app 再次通过；固件、交接及差异检查通过。
  回归包含详情身份复用和写卡失败阻止切书；日志 `.local/inspect-*.log`。未刷机。

## 2026-10-07 · 阅读响应性专项：分阶段指标与日志分位数

- 缓存预算已提交 `8b96f48`。正文请求区分位图命中、同页复用、未命中和图片加载提示，全部记录耗时；
  其他慢阶段输出自己的记录阈值，避免用截断分布宣称 P95/P99 达标。
- ZIP 资源解析记录文件读取总量/单次最长、XML 处理总量/最长块、其他解压与调用开销、取消检查最大间隔。
  XML 指标包含解析回调，均为墙钟时间；不宣称纯解压 CPU 时间，也不把父子阶段相加。
- 串口状态增加触摸任务剩余栈与队列溢出累计数；内容和网络任务分别记录剩余栈。
  应用帧完成字段由 `save` 改为 `complete`，后台进度提交仍独立计时。
- 新增 `tools/summarize_reader_perf.py`，分别汇总排队/未排队输入、软件准备、屏幕阶段及内存最小观测值。
  使用 100 条合成输入验证 P95/P99、单位和分组；合成数据不作为设备成绩。验收矩阵补齐实体输入、资源、
  映射、显示和按阅读行为测功耗的口径。
- host 与 ASan/UBSan CTest 各 11/11，固件、交接及差异检查通过；日志 `.local/perf-*.log`。
  未刷机，没有新增真机时延、内存或功耗结论。

## 2026-10-07 · 阅读响应性专项：可选缓存回收预算

- 图片与相邻分段预取已提交 `49e6911`。可选驻留缓存增加 4 MiB 总量目标，同时观察 PSRAM 空闲量和最大连续块。
  依次释放不可见封面、远处章节索引、下一页预取，再按每次最多 32 个字形平滑回收；保留当前章、已显示画布、
  已确认位置和保存快照。压力下暂停统计，避免刚回收又重算。
- `cache_total/cache_target/cache_evictions` 与分项容量同时输出。总量是已知缓存载荷的保守统计，
  不包含分配器、哈希节点、FreeType 内部对象和工作任务瞬时副本；实际 RAM、PSRAM、连续块与栈指标仍须一起看。
  该目标不是全堆硬预留，也不代替任务启动前的峰值准入检查。
- host 全套 11/11，固件、交接及差异检查通过。sanitizer 首次 app 输出通过后被 CTest 记为超时；
  单独重跑约 24 秒通过，最后补齐在途统计取消后 app 的 host/sanitizer 再次通过。其他 sanitizer 用例均通过。
  日志 `.local/cache-budget-*.log`；没有把首次超时记录改写成全套成功。
- 字形分批回收后像素保持一致。未刷机；回收阈值、任务栈与真实峰值内存仍待硬件验收。

## 2026-10-06 · 阅读响应性专项：图片与相邻分段单页预取

- 分页缓存后台读写已提交 `89d4f8e`。单页预取扩展到下一张正文图片，以及分段/章节边界后的第一页；
  相邻分段由独立解析器加载，只保留一段源内容及一页布局/位图，不连续展开后续内容。
- 命中时先转移相邻内容所有权，再沿用正常正文提交；未成功呈现时仍不推进位置。版式、代次与目标分段
  不匹配时保留原加载路径。图片预取复用灰阶磁盘缓存，翻页命中不再经过加载提示或重复解码。
- 相邻内容保守容量限制 2 MiB，布局 256 KiB，位图仍为一份；任务启动前增加 PSRAM 余量要求。
  诊断新增相邻内容容量，包含容器和字符串容量，内联字符串可能重复计入，不含分配器开销。
- host、ASan/UBSan CTest 各 11/11，固件、交接及差异检查通过；新增相邻分段命中正文帧、锚点/像素一致、
  图片命中不再解码检查。日志 `.local/adjacent-*.log`。未刷机，额外章节与字体的峰值内存仍需测量。

## 2026-10-06 · 阅读响应性专项：分页缓存后台读写

- 书库封面批次已提交 `18d4183`。分页缓存读取、完整性校验、容器展开以及序列化/写卡迁到内容任务；
  主循环只构造小型身份头并采用匹配代次的结果。同步 host 路径复用同一份编解码函数。
- 完成章节统计后在空闲时提交派生索引。固件切书、返回书库、休眠和 USB 交接不再为可重建分页缓存同步写卡；
  来不及落盘的统计允许重算，阅读进度仍按独立快照提交。
- 保留书籍/字体强身份、分页版本、校验和、严格递增边界和展开后 1 MiB 上限；工作副本准入检查额外保留
  PSRAM 余量，取消及切换版式后旧索引不覆盖新状态。缓存写失败不阻止阅读，也不持续重试占用后台。
- 新增异步开书后再采用匹配索引的检查，现有损坏缓存与运行时展开上限检查继续通过。
  host、ASan/UBSan CTest 各 11/11，固件、交接及差异检查通过；日志 `.local/page-cache-worker-*.log`。
  未刷机；索引快照复制及任务峰值内存仍需真机测量。

## 2026-10-06 · 阅读响应性专项：可取消书库封面批次

- 内容块前缀缓存已提交 `6349e0b`。固件卡片书库先显示书名及占位图，最多六张可见封面在内容任务中
  逐张提取、解码并复用现有磁盘缓存；整批完成才合并为一次屏幕更新。
- EPUB 封面提取贯通取消回调。离开书库、切换页或存储交接会取消旧批次，代次不匹配的结果不能重画当前界面。
  K2 保持当前画面全刷。进度保存优先，暂停的可见封面批次在保存后继续准备。
- 封面使用独立画布，内存不足时保留占位内容；没有把主循环画布交给工作任务。睡眠全屏封面和详情页仍使用
  原来的按需路径，后续继续处理；本批只覆盖书库卡片中连续生成多张封面的阻塞。
- 新增两张封面合并一次更新、生成期间 K2、切到设置后旧结果不重画的回归。
  host、ASan/UBSan CTest 各 11/11，固件、交接及差异检查通过；日志 `.local/cover-worker-*.log`。
  未刷机；大封面取消时延和 SD 长尾仍待真机验证。

## 2026-10-06 · 阅读响应性专项：保留已解析的前缀内容块

- 后台快照保存已提交 `780a18f`。Reading 解析增加同步的完整分段访问回调，缓存层在寻找后段时
  同时保存已完成的前段；仍只驻留一段正文，不展开整章或整书。命中后随机读取规范化内容及源锚点。
- 访问回调保留原分段的文本、段落、链接、图片和片段目标；Mapping 仍读取原书，不采用简化缓存。
  缓存写入失败不影响当前阅读，取消停止后续解析；单次 SD 提交仍需独立测量。
- KOReader 参考工具遍历所有分段，反向书签恢复按源锚点查找分段。默认逐字符检查保持不变；
  `--sample-step` 可对长书明确抽样，并额外检查每段末尾。
- host、ASan/UBSan CTest 各 11/11，固件构建、交接检查和差异检查通过；前段缓存逐字符源锚点与原解析一致。
  日志 `.local/prefix-cache-*.log`。
- 本机固定 CREngine 对照通过：原小样本逐字符；`multi-part.epub` 以 4096 字符步长抽样，覆盖第一章三段及
  第二章，在 DOM 20240114/20260812、28/38/56 px 下每组 43 个位置正反向及引擎书签全部通过。
  日志 `.local/mapping-baseline.log`、`.local/mapping-parts.log`；这是桌面引擎对照，不是真机双向接续验收。

## 2026-10-06 · 阅读响应性专项：后台进度快照提交

- 下一页位图已提交 `527f602`。空闲保存与工具面板呈现后的保存使用不可变 `SaveRequest`，
  工作任务沿用 FileStore 的临时文件、同步、回读和备份提交，不直接覆盖权威进度。
- 提交期间允许继续翻页；主循环只确认任务对应的旧修订，新页仍保持待保存。错误回包显示保存失败，
  不更新已保存位置。最近阅读文件也由此任务提交，避免正文帧完成后立即阻塞写卡。
- 保存优先于预渲染和统计；任务单槽合并后续保存需求。切书、休眠、上传和存储交接仍等待提交完成，
  这些生命周期边界的同步等待尚未消除，不能把普通阅读异步保存描述成所有 I/O 均已异步。
- 新增连续翻页期间旧快照确认与侧车目录写入失败的回归。host、ASan/UBSan CTest 各 11/11，
  最后调整最近阅读失败重试节流后，两套 app 用例再次通过；固件构建、交接检查及差异检查通过。
  日志 `.local/save-worker-*.log`。未刷机；SD 实际提交长尾和任务内存峰值仍待真机测量。

## 2026-10-06 · 阅读响应性专项：下一页正文位图

- 下一页布局在独立字体实例上绘制到额外一份 415,872 字节画布；任务完成后才采用结果，
  代次、书籍和页起点不匹配时丢弃。前台命中后复用正文像素，再生成页脚与交互区域。
- 光栅化逐字检查取消，工作任务从不写入正在显示的画布。当前仅预绘同一分段的下一张文字页，
  完成后停止；内存不足或任务启动失败时回退到布局与字形预热。
- 可选任务增加内部 RAM 准入检查，位图增加 PSRAM 总量及连续块检查。诊断输出新增位图容量和命中次数；
  这些是缓存上限与准入措施，不代表已建立全系统内存预留。
- `cmake --build build/host --parallel 8`、host CTest 11/11；sanitized 构建及 CTest 11/11；
  `sh tools/build_firmware.sh`、`python3 tools/check_handoff.py`、`git diff --check` 均通过。
  日志 `.local/bitmap-{host,sanitized,firmware,handoff}.log`。测试检查位图命中、正文像素一致和 K2 保留缓存。
- 未刷机；命中时延、峰值内存和实际功耗待真机验收。跨分段及图片预取、保存和封面同步路径继续处理。

## 2026-10-06 · 阅读响应性专项：跨章与跳转工作任务

- 可取消开书已提交 `5ee8d80`。跨章、跨分段、未命中的长距离回翻、目录、脚注及同步候选跳转使用
  内容任务加载章节或查找源锚点；回翻扫描使用同一分页函数，并返回最近 64 个计算边界供连续回翻复用。
- 已核验的标准分页索引可直接提供上一章/分段末页起点；带脚注返回栏时按实际正文高度重新排版。
  完成任务只替换待显示章节，仍由正文帧成功呈现推进位置。取消恢复已确认的脚注返回状态。
- 前台任务取消与替换保持单槽、代次检查；错误页不再为恢复背景而同步加载尚未在内存中的章节。
  异步开书的入口合并为一次加载提示，避免沿用同步 defer 再多刷一帧。
- 新增跨分段边界前后翻、跨章回翻、目录跳转及跨分段脚注返回的同步/异步锚点与正文像素对照。
  host 与 ASan/UBSan CTest 各 11/11，固件构建成功；加载文案子集检查继续通过。
  日志 `.local/chapter-worker-host.log`、`.local/chapter-worker-sanitized.log`、`.local/chapter-worker-firmware.log`。
  当前单页绘制、下一页位图、库封面、保存及部分配置/页数缓存 I/O 仍待后续迁移或复用；真机未验收。

## 2026-10-06 · 阅读响应性专项：可取消开书与恢复

- 正文图片任务已提交 `5b9ec8c`。冷开书的强摘要、ZIP/元数据和恢复章节读取移入内容工作任务；
  哈希每 8 KiB、XML 每输入块检查取消。查找恢复分段沿用内容缓存与源锚点，具备同样的取消入口。
- 任务完整返回后才转移解析器所有权并建立新会话；身份未确认时不采用或覆盖新书进度。
  开书前后核对文件属性，受控会话内仅复用已验证身份。取消保持旧会话，返回书库；旧结果不能替换新书。
- 加载状态具有独立视图，避免从休眠画面开始加载时继续受睡眠输入屏蔽。固件唤醒后重新核验书籍身份，
  不把休眠前的解析器身份当作外部存储仍未变化的证明。
- 同一个前台任务槽只保留最新请求，旧任务退出后启动新请求，不在快速切书时等待其整个解析完成。
  K2 和屏幕取消继续由主循环处理，输入未处理时暂缓应用任务结果。
- 验证：host 与 ASan/UBSan CTest 各 11/11，固件构建成功；日志 `.local/open-worker-host.log`、
  `.local/open-worker-sanitized.log`、`.local/open-worker-firmware.log`。新增异步后段恢复的锚点/正文像素对照，
  以及唤醒重新核验身份、加载视图 K2 和同会话重复开书不重扫摘要检查。
- 下一步仍包括跨章/目录读取、下一页位图以及其他同步 I/O：旧页进度提交、字体打开、分页缓存采用和
  首次页面排版尚在应用任务；不能宣称所有长路径已消除。设备时延和峰值内存未验收。

## 2026-10-06 · 阅读响应性专项：正文图片工作任务

- 图像取消接口已提交 `c2535f7`。工作任务扩展为 `ContentJob`，复用同一生命周期管理处理分页、字体身份和正文图片。
  首次正文图片先呈现加载状态及“取消”控件，随后任务读取灰阶缓存或提取、解码并保存派生像素。
  前台只接收匹配书籍操作代次、章节和分段的结果，正文成功显示后才推进进度。
- 图片准备期间 K2 仍立即请求当前画面全刷；取消返回书库，旧任务退出后丢弃结果。
  切书、显示失败、缓存清理及存储交接都取消或等待相关任务，缓存写入不触碰阅读记录。
  错误页在图片任务尚未退出时不重画图片背景，避免并发重写同一个缓存临时文件。
- 图片任务等待统计任务退出后才开始，复用现有 32 KiB 工作栈设置；准备前释放可选正文快照和封面缓存。
  当前同页正文快照可直接复用，其他图片缓存命中仍经过加载状态；独立下一页位图尚未接入。
  运行中保持现有性能频率，完成后恢复原空闲策略；没有修改波形、面板保持窗口或电源校准。
- host 与 ASan/UBSan CTest 各 11/11，固件构建成功。新增高压缩 PNG 跨解压窗口用例后，
  host 全套及 sanitizer 图形用例继续通过；日志 `.local/image-worker-host.log`、
  `.local/image-worker-sanitized.log`、`.local/image-worker-firmware.log`。
- host 测试覆盖加载期间 K2、取消后进度不变、缓存再次显示不解码、切书后旧结果及显示失败。
  真机首次图片时延、取消时延和双任务资源峰值仍待验收；书库封面与睡眠图首次准备仍在原同步路径。

## 2026-10-06 · 阅读响应性专项：图像取消检查点

- 强身份分页缓存已提交 `edce927`。PNG 按小块输入推进，保留尚未消费的头部/调色板字节；
  JPEG 在输入及 MCU 输出块间检查取消。EPUB 图片提取也改为可取消回调。
- 取消或解码失败均不修改调用方画布；仅在完整解码后提交像素。没有修改上游解码器源码或降低灰阶质量。
- host 与 ASan/UBSan CTest 各 11/11，固件构建成功；日志 `.local/image-cancel-host.log`、
  `.local/image-cancel-sanitized.log`、`.local/image-cancel-firmware.log`。
  现有图像用例验证 PNG/JPEG 取消后的画布不变，原透明、裁剪、交错 PNG 和 JPEG 检查继续通过。
- 此批只增加取消接口，应用尚未接入独立图片任务；输入到取消退出的真机时间仍未验证。

## 2026-10-06 · 阅读响应性专项：分页缓存强身份

- 分页预算与诊断已提交 `4bbc5f6`。页数缓存升级为 `PPPAGES3`，绑定 EPUB SHA-256、字体 SHA-256、
  内容版本、独立分页算法版本及完整几何；载荷增加完整性校验，重复页起点视为无效。
  已验证的磁盘索引可以服务标准分页边界的回翻；目录/脚注任意起点仍使用实际访问历史。
- 固件首次字体摘要计算在统计工作任务中执行，每 8 KiB 检查取消；完成前不采用旧页数缓存。
  内置字体摘要由构建系统从实际字体字节生成。受控会话内相同 Font 继续复用身份，字体重载和存储失效时清除。
  host 不承诺独占文件，因此重开时重新加载字体和核验摘要。
- 统计任务栈调整为 32 KiB，与主任务相同，为字体哈希和解析调用保留空间；仍需真机核对最低剩余栈。
- 新增同名字体内容变化但大小/时间不变的缓存失效检查；原分页缓存预算测试改为带有效完整性摘要的异常载荷。
- 验证：host 与 ASan/UBSan CTest 各 11/11，固件构建成功；日志 `.local/strong-key-host.log`、
  `.local/strong-key-sanitized.log`、`.local/strong-key-firmware.log`。未执行真机测试。
- 字体校验完成后的页数缓存读取仍在应用任务，尚未把该次 SD 读取与索引反序列化迁出；冷开书哈希也仍同步。

## 2026-10-06 · 阅读响应性专项：分页容器预算与缓存诊断

- 独立统计任务已提交 `780e722`。运行时分页索引按各层 vector 容量计算，增长前检查 1 MiB 预算；
  超出后停止统计，阅读位置和当前正文不受影响。磁盘加载先验证容器展开量，避免小文件声明大量空分段。
- 页数缓存保存先计算完整载荷长度，超限时不构造临时字符串；合格载荷一次预留所需空间。
  容器分配器自身的管理开销，以及重新分配期间新旧内存同时存在的峰值，仍不包含在载荷预算内。
- 诊断状态增加正文快照、下一页布局、页索引、封面像素容量，以及 UI/正文字形像素和字宽/字形条目数。
  原有内部 RAM、PSRAM、各自最大连续块与主任务栈指标保持；统计任务退出记录自己的最低剩余栈。
- 验证包括异常分段数量缓存拒绝和正常统计容量检查。host 与 ASan/UBSan CTest 各 11/11，固件构建成功；验证日志：
  `.local/budget-host.log`、`.local/budget-sanitized.log`、`.local/budget-firmware.log`。
- 各项准入仍是保守容量检查，尚未实现跨任务内存预留；真机峰值、调度延迟与功耗未验收。

## 2026-10-06 · 阅读响应性专项：独立统计工作任务

- 身份复用与轻量分页已提交 `68de2fd`。固件默认使用一个按需启动的统计任务，独占解析器和 Font，
  与前台共享分页函数；不访问会话、正文画布或前台 FT_Face。host 可显式启用相同路径。
- 请求包含书籍强身份、版式快照及代次；结果仅为页边界和分段游标，应用任务核对代次和游标后接收。
  输入采样队列有事件或接触进行中时请求取消，已完成页边界可接收并从游标继续；切书和改版式丢弃旧代次。
- 每份结果最多 16,384 个页起点；采用既有 PSRAM 准入检查。ESP 任务栈为 12 KiB，退出记录最低剩余栈。
  USB、导入、书籍管理、移卡及休眠先停止工作任务，防止文件所有权交接后仍访问旧文件。
- 完成统计后不在接收结果的路径同步写缓存，缓存仍在既有保存入口提交。首次解析后段仍可能重复前缀；
  取消检查不能中断正在进行的单次 SD I/O，存储交接会等待该操作退出。
- host 与 ASan/UBSan CTest 各 11/11，固件构建成功；日志 `.local/worker-host.log`、
  `.local/worker-sanitized.log`、`.local/worker-firmware.log`。现有 app 测试检查独立统计与同步结果一致，
  统计期间画布和进度不变，以及切书后的旧结果处理。未执行实体输入与双任务峰值内存验收。
- 前台开书、图片首次解码、跨章读取及部分缓存提交仍同步；下一页预热也仍在应用任务，专项尚未完成。

## 2026-10-06 · 阅读响应性专项：轻量分页统计

- 阅读、回翻边界扫描和统计共用 `layout_page`；后两者不再构建字形与行列表。
- 排版在行间及每 64 个测量字符检查取消；取消结果不推进页末，也不标记统计失败。
  检查间隔不等于硬性时间上限，单次字体读取和 SD 操作仍可能阻塞。
- host 与 ASan/UBSan CTest 各 11/11，固件构建成功；日志 `.local/count-light-host.log`、
  `.local/count-light-sanitized.log`、`.local/count-light-firmware.log`。现有图形测试核对两种模式页边界一致。
- 尚未将统计移出应用任务，未执行真机延迟测试。

## 2026-10-06 · 阅读响应性专项：受控会话内身份复用

- 正文/图片复用与字体淘汰已提交 `191a49f`。
- 固件显式启用受控存储模式；同一独占会话内至多缓存 8 个已完整核验的 SHA-256/部分 MD5。
  同一路径且大小、时间和 inode 不变时复用已确认摘要，仍解析 ZIP 结构和元数据。
  属性匹配只是拒绝复用的检查，不等同于强摘要。host 默认禁用复用，允许外部编辑文件。
- 导入期间、书籍删除/重置管理、USB 交接、移卡和休眠前清除身份缓存；冷启动没有缓存。
  复用只发生在调用方保证文件未被外部修改的独占阶段，未确认身份时没有恢复/保存旧记录的新通路。
- host 11/11：同一受控会话重复开书不增加整书哈希次数，显式失效后重新哈希且进度不变。
  命令与日志：host/sanitized CMake+CTest，`.local/identity-host.log`、`.local/identity-sanitized.log`；
  固件 `sh tools/build_firmware.sh`，`.local/identity-firmware.log`；上述检查全部通过。
- 首次冷开书仍完整哈希；尚未将冷开书哈希迁移到工作任务。不启用跨重启弱指纹信任。

## 2026-10-06 · 阅读响应性专项：正文/图片复用与字体淘汰

- 内容块缓存已提交 `cca66c2`。新增一份当前正文画布（415,872 字节），同页工具/面板复用正文、链接热区与版式；
  页脚仍从当前状态重画。字体/章节/版式变更、休眠与移卡等入口清除快照；内存不足回退原绘制路径。
- 正文图片在 `PicoPage/cache/images/gray-v1` 保存目标几何的 4 位灰阶像素，键包含强书籍身份、资源及几何；
  校验失败回原图解码。没有改变图像缩放或灰阶质量，也没有缓存错误占位图。
- 工具先呈现，再提交已显示位置；提交前仍显示“尚未保存”，保存失败进入错误页。
  同步徽标读取内存中的配置快照；初始化、开书、同步操作及手机配置结束刷新快照，实际同步仍重新验证文件。
- 字宽满 8,192 条后逐条 FIFO 淘汰；字形按 LRU 淘汰，不再排序并整批清空。复用已有容量限制。
- host 11/11、ASan/UBSan 11/11；`sh tools/build_firmware.sh` 成功。
  日志 `.local/body-cache-host.log`、`.local/body-cache-sanitized.log`、`.local/body-cache-firmware.log`。
  检查包括工具开关像素、醒来及重开同图不增加解码次数、显示工具前不提前确认保存、字体淘汰后字宽和像素相同。
- 未测真机延迟、缓存命中率和峰值；灰阶缓存读写及进度提交仍同步，首次图像解码尚需可取消调度。

## 2026-10-06 · 阅读响应性专项：内容块缓存

- 回翻分页边界复用已提交 `22cdf84`。用户授权后续按批验证并提交，不推送或烧录。
- 新增 Reading 内容块的有界二进制缓存，保留规范化文本、块、源锚点、链接、图片和片段目标。
  以原 EPUB SHA-256、内容格式版本、spine 序号和分段号命名；最后一段另有别名。
  缓存带完整性摘要和字段/长度校验；Mapping 仍读取原 EPUB，不以缓存冒充 KOReader DOM。
- 普通分段读取可直接命中；按锚点/目录查找先检查连续已缓存分段，缺失时回原解析器。
  缺失、截断、版本不符或内存不足都回退原路径。保存复用 FileStore 的临时文件发布，不写阅读记录。
  每条最多 2 MiB，缓存读写前保留至少 4 MiB PSRAM 与 2 MiB 连续块，并遵守解析器内存探针。
- 限制：首次未命中的后段仍解析前缀；缓存写入仍同步，未改成后台提交。缓存校验摘要不表示恶意篡改认证。
- 现有 app 测试新增后段逐字符源锚点对照、截断缓存回退重建、取消及低内存路径；host 11/11。
  ASan/UBSan 11/11，`sh tools/build_firmware.sh` 成功。日志：`.local/content-cache-sanitized.log`、`.local/content-cache-firmware.log`。
  host 命令为 `cmake --build build/host --parallel 8` 和 `ctest --test-dir build/host --output-on-failure`。
  无真机测试，未声称达到延迟目标。

## 2026-10-06 · 阅读响应性专项：回翻分页边界复用

- 下一页布局与字形预热已提交为 `cc698c6`，未推送；工作区原有 BSP 更新继续保留。
- 回翻统一先查最近成功显示页，再查近期计算边界，最后查本次会话完成的标准分页索引。
  精确页跨度只有在结束位置等于当前起点时才复用，不再把任意更小的已访问起点当作上一页。
  标准索引只用于普通正文高度，且目标必须是标准边界或分段末尾；目录/脚注任意位置与返回栏
  保留独立的短期页跨度，不直接套用全书统计的页起点。
- 实际访问页在正文呈现成功后记录，显示失败不写入访问历史；实际页与计算页分别最多保留 64 条。
  切书、改版式、目录/脚注跳转及现有清理入口同时清除两者，记录包含章节、分段和返回栏状态。
  未命中时沿用按当前版式重新分页的路径，仍保留最近计算的边界以服务紧接着的回翻。
- `counted_live` 区分本次打开期间完成的统计与磁盘加载的页数缓存。
  磁盘页数缓存没有强字体身份，本批仍只用于页码显示，不把它升级为正文导航依据。
  本批没有改缓存文件格式，也没有改变阅读记录或同步锚点。
- 测试：现有跨分段 200 多页前后锚点对照通过；新增目录后段跳转后前翻/回翻的锚点与像素一致性，
  并核对随后 K2 不改位置。首轮暴露丢弃计算边界使连续回翻重新进入排版提示，已补充有界计算缓存并重跑。
- 实际执行：host 构建与 CTest 11/11；命令 `cmake --build build/host --parallel 8`、
  `ctest --test-dir build/host --output-on-failure`，日志 `.local/page-index-host.log`。
  `cmake --build build/sanitized --parallel 8` 后对应 CTest 11/11，无 sanitizer 报告；
  `sh tools/build_firmware.sh` 成功。日志 `.local/page-index-sanitized.log`、`.local/page-index-firmware.log`。
  `python3 tools/check_handoff.py` 与 `git diff --check` 通过。
- 本批未提交、推送或烧录。未测真机时延和资源峰值；跨分段读取仍从 XHTML 入口重新解压解析，
  缓存未命中的分页仍同步执行，不能宣称数秒级卡顿已全部消除。
- 下一步：在保留源锚点的条件下实现可随机读取的内容块；同时补齐内容、字体和分页算法的缓存身份，
  再考虑将磁盘分页索引用于正文导航。长任务工作线程与可恢复解压仍未完成。

## 2026-10-06 · 阅读响应性专项：下一页布局与字形预热

- 统计协作式取消已按用户要求提交为 `ef8f44f`，未推送；原有 BSP 更新仍单独保留在工作区。
- 空闲工作先准备当前分段内的下一页文字布局，再分批预热字形，完成后继续全书页数统计。
  布局、字形与前台共享算法和 Font 缓存，仍由同一个应用任务调用；没有新增线程或并发 FT_Face 访问。
  前翻可直接取用预先排好的 Page，正文像素仍在实际翻页时绘制。
- 仅保留一页布局；矢量容量合计超过 256 KiB 时放弃保留。固件在布局前检查至少 2 MiB 空闲 PSRAM
  和 256 KiB 连续块，字形预热批次前检查 2 MiB 空闲及 64 KiB 连续块；不满足时放弃预取。
  字形复用现有每个 Font 约 1 MiB 的缓存上限。上述是准入检查及保留量限制，不是系统峰值内存保证，
  未加入额外整屏位图。页末跨分段、跨章和下一页图片暂不预取。
- 字形间检查输入取消，批次目标 15 ms；已完成的预热游标可在下一批继续。
  单页排版、单字光栅化和缓存淘汰仍可能超过时间目标。预取不修改当前画布、不提交帧、不推进或保存进度。
- 切书、换分段、字体重载、版式重置和离开阅读等入口清除预取；消费时核对起点及全部 LayoutSpec 字段。
  K2 全刷保留预取，避免重复全刷使统计不断重做准备。预取失败只回退前台原路径，不显示错误页。
- 验证：现有 app 测试比较预取翻页与冷路径翻页的像素，验证取消预热后画布/记录不变、
  统计最终完成、页数缓存命中后仍预取，以及重复 K2 不阻止统计。graphics 测试比较独立 Font
  预热后的字形与直接光栅化的像素。首次测试发现 K2 清空预取导致统计无法推进，已修复并重跑。
- 实际执行（仓库根目录）：host 与 ASan/UBSan 构建后 CTest 各 11/11，通过；
  命令为 `cmake --build build/host --parallel 8` / `cmake --build build/sanitized --parallel 8`
  及对应的 `ctest --test-dir ... --output-on-failure`。日志 `.local/prefetch-host.log`、
  `.local/prefetch-sanitized.log`，测试明细在构建目录 `Testing/Temporary/LastTest.log`。
- 固件验证：`sh tools/build_firmware.sh` 成功，日志 `.local/prefetch-firmware.log`。
  `python3 tools/check_handoff.py` 与 `git diff --check` 通过。
- 本批未提交、推送或烧录；真机输入延迟、预热收益、字体缓存命中率与内存峰值均未测。
  开书、跨章和图片的同步阻塞仍存在，工作任务迁移及可恢复解压尚未完成。
- 下一步：测量该路径的设备收益与资源余量，并继续消除跨分段重复解析；整页位图和跨章预取须另行控制预算。

## 2026-10-06 · 阅读响应性专项：统计协作式取消

- 第一批诊断与输入溢出恢复已按用户要求提交为 `1f34bc4`；未推送。
  提交未纳入工作区已有的 BSP 更新及其开发记录。
- 第二批接入 `background_step(stop)`：固件检查采样队列是否存在待处理事件。
  统计在入口、页间和章节解析检查点返回主循环；XML 每次最多送入 4096 字节后检查取消。
  分页批次目标由 40 ms 调整为 15 ms。解析器、正文 FT_Face、画布仍由应用任务独占，
  本批没有增加并发访问或新工作任务。
- `load_chapter` 的同步取消回调返回真时释放输出，用独立错误常量报告取消。
  统计不会将取消标成失败，也不会丢弃已完成的分页边界；空闲后可继续统计。
  未完成的章节解析没有保存解压游标，下次仍从资源入口重读；频繁输入可能推迟统计完成。
- 验证：现有 document 测试补充多段章节后段的中途取消、重新读取和源锚点一致性；
  app 测试补充统计入口/页间取消后画布、保存记录不变，随后仍完成统计。
  保留原有长章节拼接、分页缓存和排版变更检查，没有新增测试框架。
- 实际执行（仓库根目录）：
  - `cmake --build build/host --parallel 8`、`ctest --test-dir build/host --output-on-failure`：11/11。
  - `cmake --build build/sanitized --parallel 8`、`ctest --test-dir build/sanitized --output-on-failure`：11/11，无 sanitizer 报告。
  - `sh tools/build_firmware.sh`：成功。
  - 日志：`.local/responsiveness-cancel-host.log`、`.local/responsiveness-cancel-sanitized.log`、
    `.local/responsiveness-cancel-firmware.log`；CTest 结果在各构建目录的 `Testing/Temporary/LastTest.log`。
- 限制：15 ms 不是硬时间上限。同步 SD I/O、miniz 单次解压、单页排版和最终缓存提交仍不可中断；
  开书、正文图片及 `defer()` 尚未迁移。当前取消来源是触摸/电容键采样队列，
  PMU 电源键、诊断串口仍由主循环轮询。没有声称达到 P95/P99 目标。
- 本批未提交、推送或烧录；实体输入延迟、取消检查点最长耗时及峰值内存仍需真机验证。
- 下一步：拆出可恢复的内容计算状态并明确工作任务交接；继续保护当前页与保存版本，
  再将下一页准备放在全书统计之前。未增加内容缓存前，取消后的重复解析成本仍存在。

## 2026-10-06 · 阅读响应性专项：第一批诊断与输入溢出恢复

- 对照审阅意见检查当前实现：`count_pages()` 的 40 ms 截止时间不覆盖章节加载，
  单页排版完成后才检查截止时间；完成统计时仍可能同步写缓存。本批没有将其描述为硬时间上限。
- 输入队列写入失败时增加累计溢出日志，清空积压并发布 `InputReset`，取消全部旧接触；
  采样器等待松手后再接受新手势。主循环清除接触序号与抑制状态，`ReleasedTap` 清除配对状态。
  溢出时不补执行积压翻页；正常队列下的完整手势与刷新期间翻页合并规则不变。
- 新增 `reader_perf` 慢阶段日志：EPUB 打开、身份读取/哈希、XML 读取/解压/解析、
  正文与统计章节加载、逐页排版、正文准备/绘制、图片读取/解压与解码/绘制、
  进度提交和页数缓存提交。只输出耗时至少 20 ms 的调用，单位为微秒；
  父阶段包含子阶段，不能相加。沿用主循环已有的输入等待/准备时间及面板计时。
  这些是慢调用定位日志，不是完整样本分布，不能用来计算全部操作的 P95/P99。
  XML 内部读卡、解压、解析仍为合并计时，单步骤最大值与资源峰值采样尚待补齐。
- 实际执行（仓库根目录）：
  - `cmake --build build/host --parallel 8`、`ctest --test-dir build/host --output-on-failure`：11/11。
  - `cmake --build build/sanitized --parallel 8`、`ctest --test-dir build/sanitized --output-on-failure`：11/11，无 sanitizer 报告。
  - `sh tools/build_firmware.sh`：成功；日志 `.local/responsiveness-firmware.log`。
  - 构建日志另见 `.local/responsiveness-host.log`、`.local/responsiveness-sanitized.log`；
    CTest 结果见对应构建目录的 `Testing/Temporary/LastTest.log`。
- 在现有 `core_test` 增加一组取消边界检查：旧接触的抬手不产生动作，随后新 K2 手势可正常完成。
  该检查不覆盖 FreeRTOS 队列调度、真实触摸或实际面板全刷。
- 保留本轮开始前未提交的 BSP 更新；未提交、推送或烧录。没有修改存储提交协议、书籍身份、
  源位置、K2 映射或波形。现有数秒级同步阻塞仍存在，不能宣称已达到响应性目标。
- 未测：真实队列溢出后恢复保存/休眠、日志开销、实体输入延迟分布、内部 RAM/PSRAM 峰值。
- 后续按小批次推进：补齐分阶段与资源测量；将长任务移出 UI 路径并支持取消；
  下一页布局/字形预热；内容块与分页索引复用；受控存储会话内身份复用；
  图片/正文画布复用和缓存预算。工作任务迁移须先落实解析器与字体的单一所有权、
  结果代次检查和在途显示缓冲不可变，再验证切书、取消、显示失败及保存失败场景。

## 2026-10-06 · GitHub 私有仓库

- 按用户要求创建私有仓库 `https://github.com/eMUQI/pico-page`，配置 `origin`，推送 `main` 并建立跟踪关系。
- 实际执行：`gh repo create --private --source=. --remote=origin --push`；`gh repo view` 确认 `PRIVATE`，
  `git ls-remote origin refs/heads/main` 与本地提交 `5ffbb59b1765097adaf1c1868dbb59786dff5f70` 一致。
- 上传前检查完整 Git 历史中的常见凭据模式、敏感文件名和超过 50 MiB 的文件，均未发现匹配项；
  本地个人媒体、设备日志和构建产物继续由 `.gitignore` 排除。
- ChatGPT GitHub 连接账号为 `eMUQI`，安装采用仅选定仓库授权；读取新仓库的 `README.md` 返回 404。
  GitHub 授权设置要求用户重新验证身份；仓库读取权限尚未验证成功。
- 本批未修改固件或运行产品测试。下一步：将新仓库纳入 ChatGPT Connector 授权范围，再实际读取文件验证。

## 2026-10-06 · 合并上游 BSP 的 VCOM 门控（仅构建，未安装）

- 起因：硬件厂商通知参考固件发布了显示效果更新，上游提交 `5f3406d`
  “feat: enhance VCOM control during scanning”，用于改善阳光下发灰。
- 核对：该提交只修改 `components/read_pico/read_pico_board.c`（18 行），其余八个组件未变。
  内容、对本项目的影响和与工厂 VCOM 值的关系见 [BSP 审查](bsp-review.md) 的“VCOM 门控”。
- 修改：BSP 锁定版本由 `28cde68` 升到 `5f3406deec33304de8801046db583dec7c49e55b`
  （`tools/setup_bsp.sh`、`firmware/CMakeLists.txt` 及相关文档）。没有修改本项目的显示代码。
  `tools/setup_bsp.sh` 现在会把停在旧锁定版本的检出更新到新版本，此前只处理全新检出。
- 实际执行：`sh tools/build_firmware.sh` 成功（`.local/provisioning-20261006/firmware-build-vcom.log`），
  脚本输出 `Read Pico BSP: 5f3406d…`，SD 补丁正常应用。host 代码未改动，没有重新运行 host 测试。
- 产物：`.local/provisioning-20261006/pico_page-vcom.bin`，2,226,000 字节，
  SHA-256 `cceff2320a02844c05f54f73fa140eef3c041694c2a21db300d96bc51395f6c8`，**未安装**；比已安装版本大 112 字节。
- 未验证（需真机）：阳光下和室内的显示效果是否改善；局部刷新和全刷的耗时变化
  （按代码每次进入扫描多等 2 毫秒）；残影和连续翻页的观感。

## 2026-10-06 · 手机配置版本已安装（配置功能尚未在真机运行）

- 用户确认（“你烧录吧”）后写入 `.local/provisioning-20261006/pico_page.bin`，仅 `0x10000`，`Hash of data verified`
  （`flash.log`）。写入前核对：只连接一台设备，USB 序列号与芯片 MAC 均为 `…:18:B8`；设备在正文，
  进度已保存（修订号 9，第 3 / 409 页，`pre-status.log`）。
- 重启后进入书库，TF 已挂载，首帧成功；启动后可用内部 RAM 137,659 字节，最大连续块 90,112 字节，
  与安装前的数值相同（`post-status.log`）。
- 没有注入任何输入，没有开启热点，没有打开书籍。配置功能的全部真机项仍未验证，见下一条记录。
- 测试时注意：配置会话期间诊断串口的 `tap`、`key` 返回 `busy`（与同步请求期间相同），只能读 `status`；
  会话只能由屏幕上的“结束配置”、电源键或 10 分钟超时结束。

## 2026-10-06 · 手机配置 Wi-Fi 与同步账户（实现记录）

- 用户要求把配网和同步配置全部实现，之后统一测试（D19）。实现前核对的来源和取舍见 [依赖审查](dependency-review.md)。
- 新增：
  - `reader_sync/provision`：表单解析、`network.toml` / `sync.toml` 的读写规则、配置页 HTML、结果文案、加入热点的二维码内容。
    `network.toml` 的解析从板级代码移到这里，host 可测。`sync::auth_request()` 生成账户验证请求。
  - `reader_graphics`：`draw_qr()`，编码器为未修改的 qrcodegen（MIT）。
  - `reader_store`：`FileStore::save_private()` 只替换两个凭据文件，不留备份。
  - 板级：`connect_network()` 改为接收名称和密码并报告失败阶段；`start_access_point()`、`start_portal_server()`
    （HTTP 表单、未知路径跳转、DNS 应答）。说明和预算见 [板级运行说明](platform-runtime.md) 的“手机配置会话”。
  - 应用：`View::Portal` 配置页，入口在“网络与传输”和“KOReader 同步”；界面规则见 [UI 实现](ui-implementation.md) 第 13 节。
  - 主循环：网络任务运行配置会话，验证通过后才写文件；结束条件为屏幕按钮、10 分钟无提交、配置页被取代、电源键。
- 行为变化：同步请求的联网路径改为“网络任务读 `network.toml` 后调用 `connect_network(名称, 密码)`”，错误文本不变。
  “网络与传输”页布局调整，USB 按钮位置上移。
- 实际执行（仓库根目录）：
  - `cmake --build build/host --parallel 8`，`ctest --test-dir build/host`：**11/11**。
  - `cmake --build build/sanitized --parallel 8`，`ctest --test-dir build/sanitized`：**11/11**，无 ASan/UBSan 报告。
  - `.local/font-tools/bin/python tools/setup_fonts_subset.py --strings firmware/components/reader_app/app.cpp
    --strings firmware/components/reader_sync/provision.cpp --strings firmware/main/app_main.cpp`：537 个字符（原 507）。
  - `sh tools/build_firmware.sh`：成功，日志 `.local/provisioning-20261006/firmware-build.log`。
  - `python3 tools/check_handoff.py`：通过。
- 新增检查：
  - `provision_test`：表单解码与各种无效输入、userkey 与已知 MD5、写出的两个文件被固件解析器读回且字段相同、
    替换凭据文件后目录里没有旧密码和备份、手写的无效 `network.toml` 被拒绝且错误不含原文、
    配置页对扫描到的名称和已保存值做 HTML 转义、结果文案、账户验证请求。
  - `graphics_test`：二维码每个模块的位置与编码器输出逐一相同，静区为白，放不下或过小时画布不变。
  - `app_test`：两个入口只发出请求；被拒绝时不进入配置页；二维码首帧整屏刷新；配置页上 K2 保持画面，
    K1/K3、返回格和正文区点击无动作，不能休眠；“结束配置”只发一次停止请求；结束后回到设置，
    迟到的状态被忽略；有书时会话前后已保存的进度相同。
  - 桌面预览截图中的两个二维码用 macOS CoreImage 解码，得到 `WIFI:T:WPA;S:PicoPage-PREVIEW;P:preview0;;`
    和 `http://192.168.4.1`。
- 产物：`.local/provisioning-20261006/pico_page.bin`，2,225,888 字节，
  SHA-256 `08a722f8122f7af09f6f5a8ac27772570836da5fb14345d9bd40769271d62479`，已安装（见上一条记录）；比版本 E 大 103,008 字节，
  3 MiB 构建分区剩余 29%。
- 未验证（全部需要真机，板级和主循环的新增代码没有任何 host 覆盖）：
  - 热点能否启动、手机能否加入、配置页是否自动弹出（iOS 和安卓各自的探测行为）、二维码在面板上能否被扫。
  - APSTA 下连接家用路由器、对时、HTTPS、`/users/auth` 的实际结果；失败原因编号与文案是否对应。
  - 内部 RAM 最低值、HTTP 和 DNS 任务的栈余量、会话结束后内存是否回到原值、反复开关会话是否泄漏套接字。
  - 原有同步路径在 `connect_network()` 改动后的真机行为（此前同样未在真机运行过）。
  - 热点开启期间的耗电；10 分钟超时。
- 已知限制：见板级运行说明；另外表单提交失败后手机上已填的内容不保留，需要重填。
- 下一步（需要用户确认烧录并在设备旁）：安装后先从“KOReader 同步 › 用手机配置”完成一次配置并记录 `status` 的内存数字，
  再做同步检查和与 KOReader 的双向接续。

## 2026-10-06 · 配网与同步配置：方案和电脑端配置脚本（未涉及设备）

- 用户要求先做配网和 KOReader 同步配置的填写，并询问是否用 SmartConfig。结论与取舍：
  - 不用 SmartConfig：只能传 SSID 和密码，同步服务地址和账户仍需另一个入口；需要专用 App 或微信，
    且手机当时必须连在 2.4 GHz 上；原稿 S09-2 已定为设备热点加手机配置页。
  - 参考：CrossMux（检出 `3fb809f`）用屏幕键盘输入 Wi-Fi 密码和 KOReader 账户，另有网页可改设置；
    KOReader 用屏幕键盘。墨水屏逐字输入长密码很慢，屏幕键盘留作后备，本阶段不做。
  - 用户同意分两步：先用电脑写配置文件，在真机上验证现有联网路径；再做设备热点配置页。
- 本批只做第一步的电脑端部分。新增 `tools/setup_network.py`：
  - 询问 Wi-Fi 名称和密码、同步账户、服务地址和密码，写入存储卡的 `PicoPage/network.toml` 和 `PicoPage/sync.toml`。
    同步密码只以 MD5（KOReader 的 userkey）写入；已有 `sync.toml` 的 `device_id` 和 `dom_version` 保留，
    否则生成新的 `device_id`，`dom_version` 取 20260812。
  - 写入前按固件解析器的限制检查；默认从电脑向同步服务的 `/users/auth` 验证账户，被拒绝时不写任何文件，
    `--offline` 跳过验证。目标目录没有 `PicoPage` 时拒绝写入。
  - 没有改固件源码。
- 实际执行（仓库根目录）：`cmake --build build/host --parallel 8` 后 `ctest --test-dir build/host`：**10/10**；
  `build/sanitized` 同样 **10/10**；`python3 tools/check_handoff.py` 通过。
  新增 `setup_network` 测试；`sync` 测试改为用固件的 `load_config` 读取该脚本写出的 `sync.toml`
  （账户名含引号、反斜杠和中文），字段逐项相同。脚本用临时目录实际运行过一次（开放网络、跳过同步）。
- 未验证：
  - `/users/auth` 验证没有对任何真实服务运行过。
  - `network.toml` 的解析只在固件中编译，host 测试只能核对脚本的输出是合法 TOML 且符合同样的长度规则。
  - 真机上的 Wi-Fi 连接、对时、HTTPS 和 KOSync 读写一次都没有运行过；联网时的内部 RAM 最低值未知。
- 后续：用户要求先全部实现再统一测试，设备热点配置页已在上一条记录中实现；本脚本保留为电脑端的填写方式。

## 2026-10-06 · 版本 E 真机验证（已安装）

- 用户确认（“烧录吧”）后写入 `.local/pages-settings-20261006/pico_page-e.bin`，仅 `0x10000`，`Hash of data verified`
  （`flash-e.log`）；写入前核对 USB 序列号与芯片 MAC 均为 `…:18:B8`，设备在书库，修订号 258。
- 诊断输入实测《亲密关系（十周年纪念版）》（`count-e-qm.log` 及本次会话的导航输出；截图 `e-ref.png`、`e-restored.png`）：
  - 开书 5.93 秒。旧缓存作废后重新统计全书，约 31 秒内完成（每 10 秒轮询一次，含轮询造成的停顿），共 2,551 页。
    统计期间 `min_heap` 3,871,440 字节；统计后可用 4,628,719 字节，没有滞留。
  - 从目录进入“参考文献”1.52 秒，显示第 1681 / 2551 页，正文正常；前后翻页 0.73–0.86 秒。
  - 从“译者后记”首页回翻进入“参考文献”的最后一段 4.82 秒（需流式解析整章 422 KB），之后回翻 0.74 秒。
  - 全程 `min_heap` 不低于 3,579,164 字节，没有重启，主任务栈最低剩余 7,552 字节。
- 设备此前在第 226 页（修订号 258）。测试中的跳转被正常保存；测试后经目录回到原小节并翻到第 226 页，
  画面首行与测试前一致，修订号 271。
- 未验证：实体触摸下换段时的观感和状态行；多段章节上的 KOReader 同步换算；空闲电流；其他大书。

## 2026-10-06 · 章节分段读取（版本 E）

- 用户确认后写入版本 D（`flash-d.log`，仅 `0x10000`，校验通过，序列号与 MAC 均为 `…:18:B8`）。真机验证：
  从目录跳到《亲密关系（十周年纪念版）》的“参考文献”（422 KB，约 35.5 万字），设备没有重启，
  显示“本章内容过大，设备内存不足，无法显示。”，`min_heap` 1,986,908 字节；点“恢复阅读”回到第 226 页，修订号 258 不变。
  测试画面被用户看到。用户明确表示读者不会接受章节打不开（D17）。
- 该章此前从未能显示：旧版本受“单章 200,000 标量”上限限制，报读取失败或因内存不足重启。
- 本批改为分段读取，规则见 [文档运行时](document-runtime.md)：
  - 解析器一次只保留一段（约 60,000 标量，在块边界结束），原有的章节文本上限取消；XML 上限放宽为 16 MiB。
  - 阅读用 `Reading` 级别读取，不再保留文本节点原文和样式表，也不读外部样式表；同步换算位置时另行按 `Mapping` 读取目标所在的段。
  - 阅读层按“章节 + 段”定位：翻页跨段、回翻到上一段或上一章的最后一段、开书恢复、错误恢复、脚注返回、同步跳转、
    目录和链接跳转都先找到目标所在的段。页数统计逐段进行，分页缓存格式升为 `PPPAGES2`。
  - 版本 D 的解析内余量检查保留，作为最后一道保护。
- 实际执行：host 与 ASan/UBSan CTest 各 **9/9**；`sh tools/build_firmware.sh` 成功（`firmware-build-e.log`）。
  新增夹具 `multi-part.epub`（单章 5,000 段、约 14.9 万字、3 段）。
  `document_test`：三种级别分段一致，各段拼接等于全文，每个字符的锚点可往返，按锚点和片段能找到所在的段，
  超出段数的读取失败，单个超长块从块内分开；`Reading` 与 `Mapping` 的正文、分段和锚点逐项相同。
  `app_test`：在多段章节中逐页翻到书末再逐页翻回，每页锚点与去程相同、页码连续；从目录跳到后段、重启后恢复到同一页。
- host 实测：该书统计完成，共 2,307 页（此前不含“参考文献”为 1,542 页）；从目录进入“参考文献”显示第 1532 页，可前后翻页。
- 版本 E：`.local/pages-settings-20261006/pico_page-e.bin`，2,122,880 字节，
  SHA-256 `3bc0bb218ae28c20626da8f31309fd586ce71d302977590ce7f4bb536f8bcf1d`，已安装（见上一条记录）。
- 行为变化：
  - 已有的分页缓存（`PPPAGES1`）作废，每本书首次打开会重新统计一次。
  - 超过约 6 万字的章节，段与段之间的那一页可能不满。
  - 进入长章节靠后的段需要从头流式解析，耗时随位置增加，真机未测。
- 未验证（需真机）：长章节换段和跳转的实际耗时；分段后的内存最低值；统计 2,307 页的总耗时；
  KOReader 同步在多段章节上的换算（host 测试只覆盖单段样本）。
- 已知限制：`tools/` 下的 KOReader 对照工具仍只读取每章第一段；主任务栈余量约 7.5 KB；统计时每页生成完整字形列表；缓存没有总量上限。

## 2026-10-06 · 章节读取的内存余量检查（版本 D，已安装）

- 用户确认后写入版本 C：`.local/pages-settings-20261006/pico_page-c.bin`，仅 `0x10000`，`Hash of data verified`
  （`flash-c.log`）；写入前核对 USB 序列号与芯片 MAC 均为 `…:18:B8`，写入前设备在书库，修订号 258。
- 版本 C 真机实测（诊断 `open` 加每 10 秒一次 `status`，`count-c-nahan.log`、`count-c-qm.log`）：
  - 《吶喊》从未统计到统计完成不超过 10 秒，共 460 页。统计期间 `min_heap` 2,793,224 字节；
    统计后可用内存 3,967,367 字节，比统计前少约 0.1 MB，版本 B 的 1.6 MB 滞留不再出现。
  - 《亲密关系（十周年纪念版）》开书 6.06 秒，直接采用缓存，显示第 226 / 1693 页，没有重新统计。
  - 主任务栈最低剩余 7,600–7,728 字节。日志中没有超过 150 毫秒的统计单片记录（记录只在诊断轮询之间采集，不完整）。
- 本批（版本 D）：解析章节时由解析器自己检查余量，内存不足成为可恢复的读取失败，不再依赖调用方估算。
  - 每个元素和每段文本开始前检查 PSRAM 可用总量不低于 768 KiB；正文缓冲和单个文本节点原文增长前，
    按增长后的大小检查可用总量和最大连续块。不满足时停止解析，错误以 `Chapter exceeds available memory` 开头，章节内存全部释放。
  - 阅读换章：先保留当前章读取新章；因内存停止时释放当前章再读一次；仍失败则显示
    “本章内容过大，设备内存不足，无法显示。”，错误页按原有逻辑重新读取最后确认位置所在的章。
  - 开书恢复位置、同步位置换算遇到同一错误时给出对应文案；页数统计遇到时暂停，换章后再试，不记为 0 页。
  - host 没有内存限制，检查默认恒真；`set_chapter_memory_probe` 供测试替换。
- 实际执行：host 与 ASan/UBSan CTest 各 **9/9**；`sh tools/build_firmware.sh` 成功（`firmware-build-d.log`）；
  内置提示字体按新文案重新生成，507 个字符。
  `document_test` 新增：余量不足时完整和精简解析都以内存错误失败并释放章节，恢复后同一章读取结果不变。
  `app_test` 新增：换章时余量始终不足则显示错误并保留已确认位置，恢复后可继续翻页；第一次不足、释放当前章后成功则正常进入新章并可翻回。
- 版本 D：`.local/pages-settings-20261006/pico_page-d.bin`，2,113,312 字节，
  SHA-256 `00f8903415957608ef391b9c6a644c1776d70dea5dc2b10cd85767031370e889`，已安装（见上一条记录）。
- 未验证（需真机）：768 KiB 的保留量是否合适；翻到或从目录跳到超限章节时不再重启；检查本身对解析耗时的影响
  （每个元素和文本段各读一次可用内存计数，缓冲增长时遍历一次堆）。
- 仍未处理：阅读用的章节保留一份文本节点原文，只在同步位置换算时使用，约占章节内存的四成；
  主任务栈余量约 7.5 KB；统计时每页生成完整字形列表；缓存没有总量上限。

## 2026-10-06 · 资源审计与修正（版本 C，已安装）

- 用户确认（“烧录吧”）后写入版本 B：`.local/pages-settings-20261006/pico_page-b.bin`，仅 `0x10000`，`Hash of data verified`
  （`flash-b.log`）；写入前核对 USB 序列号与芯片 MAC 均为 `…:18:B8`。随后按用户要求做资源审计，
  参考以 CrossPoint（经 CrossMux 检出）和 KOReader 为主，来源见 [依赖审查](dependency-review.md)。
- 版本 B 真机实测（诊断输入，`repro-3.log`、`probe-settings.log`）：
  - 打开 12.5 MB 的《亲密关系（十周年纪念版）》并统计完成，150 秒内无重启。`min_heap` 为 1,380,096 字节（修复前 292,680）。
  - 统计结束后可用内存比统计前少约 1.6 MB 且不恢复。
  - 设置首页首次进入准备 0.50 秒，再次 0.07 秒，没有出现数秒的 FAT 扫描；存储页首次 0.82 秒，再次 0.21 秒；
    输入页 0.56 秒，同步设置页 0.69 秒。首次耗时主要是新字形。
  - 主任务栈最低剩余 7,488 字节（共 32,768），此前版本为 7,616 字节。
- 审计发现并已在版本 C 修正：
  1. 释放不彻底。设备工具链（libstdc++）下 `chapter = {}` 保留字符串容量，统计用章节的正文缓冲一直占着，
     即上面的 1.6 MB。改为交换后析构（`release(Chapter&)`），阅读侧同类写法一并改掉。host（libc++）不会出现，测试无法发现。
  2. 内存估算不是上界。版本 B 按“每字节 8 字节”估算，超限章节实际峰值 3.2 MB，超过估算的 2.4 MB。
     现在统计用的解析不保存原文副本、节点路径、锚点字符串和样式表，正文一次预留、不再倍增；
     各项限制与完整解析相同（`document_test` 逐章比对正文、块、图片和分段边界）。
     host 实测该书最大一章的峰值由 3.07 MB（完整解析）降到 1.29 MB，600 KB 的超限样本由 3.46 MB 降到 0.95 MB；
     普通 90 KB 章节约 0.44 MB。上界为 4 字节 × min(XHTML 字节数, 200,000) 加固定记录。
  3. 余量检查同时看 PSRAM 的可用总量和最大连续块：连续块需容纳正文，总量需再多出 2 MiB。
  4. 诊断：`status` 新增 `page`、`pages`、`count`、`largest_psram`；统计单片超过 150 毫秒时写日志。
- 实际执行：host 与 ASan/UBSan CTest 各 **9/9**；`sh tools/build_firmware.sh` 成功（`firmware-build-c.log`）。
  版本 C：`.local/pages-settings-20261006/pico_page-c.bin`，2,112,736 字节，
  SHA-256 `5d26f4529014d69972dee3adac06c51c7c72b625b42aa67aef933b6cf6dd144e`，**未安装**。
- 审计发现、当时尚未处理（均为此前已有；第 1 项已在版本 D 处理，见上一条记录）：
  1. 阅读侧换章没有余量检查。换章时旧章与新章同时在内存；完整解析的章节没有与设备内存匹配的上界
     （该书末章完整解析峰值 3.07 MB，阅读时可用约 2.5–3.9 MB）。翻到或从目录跳到这类章节可能重启。
  2. 主任务栈余量约 7.5 KB，最深路径未定位。
  3. 统计每页仍生成完整的字形定位列表，只为得到页起点；可改为只量行的排版以减少 CPU 和堆操作。
  4. 分页缓存和封面缓存没有总量上限，只能整体清理。
- 测量的副作用：用户当时正在使用设备。计时用的诊断点击把设备从其正文页带到了书库和设置，最后停在书库；
  阅读进度未变（修订号 255）。此后未再向设备发送输入。
- 未验证：版本 C 在真机上的内存最低值、统计耗时与输入延迟；空闲电流。

## 2026-10-06 · 页数统计导致重启（版本 B 修复，已安装）

- 用户反馈：在刚安装的版本上打开《亲密关系》，过程中设备回到书库。设备 `uptime_ms` 只有 31 秒，实际是重启。
- 复现（诊断 `open` 加每 5 秒一次 `status`，`.local/pages-settings-20261006/repro-2.log`）：
  12.5 MB 的十周年纪念版开书正常，可用内存约 3.9 MB；开书后 15–20 秒之间 `min_heap` 由 3,685,332 降到 292,680 字节，
  之后统计停止。这次没有重启，也没有采到崩溃日志；5.5 MB 的另一本同名书全程正常（`repro-1.log`）。
- 原因：页数统计为其他章节再解析一份完整章节。该书最后一章 `text00022.html` 为 422 KB，超过 200,000 个字符的章节上限，
  解析在失败前占用约 3.4 MB。固件关闭了 C++ 异常，分配失败即 abort 重启；剩余 0.29 MB 时任何一次翻页或换章都可能触发。
  host 对照确认该章读取失败（`Chapter text scalar limit exceeded`），并使整本书的统计中止。
  用户那次重启的崩溃日志没有采到，以上是按内存记录作出的判断。
- 修复：
  - 统计用的章节只在正文页面空闲时存在：任何输入、开书、休眠、USB 传输和移卡都先释放它，已统计的部分保留，空闲后重新读取并继续。
  - `EpubDocument::load_chapter` 新增 `layout_only`，不保留各文本节点的原文副本，统计用章节的内存约减半。
  - 读取前按章节大小估算内存上限（每字节 8 字节，封顶 200,000 字符，另加 768 KiB），最大空闲块不足时暂不统计，
    换到其他章节后再试。host 不做此检查。
  - 超过解析上限的章节记为 0 页，不再中止整本书的统计；其他读取失败仍中止，且不写缓存。
- 实际执行：host 与 ASan/UBSan CTest 各 **9/9**；`sh tools/build_firmware.sh` 成功（`firmware-build-b.log`）。
  `app_test` 新增：统计期间穿插 K2 后总页数不变；含超限章节的书能统计出页数。
  host 上该书统计完成，页脚显示“第 2 / 1542 页”，分页缓存 6,399 字节。
- 产物：`.local/pages-settings-20261006/pico_page-b.bin`，2,111,280 字节；
  SHA-256 `02d862d566eaea8c0b63fc886d9becd1ee1e88ecf745ebb8838de6c057d5dc3a`。
  第一次写入命令被权限检查拒绝；用户再次确认后已安装，见上一条记录。
- 复现的副作用：两本《亲密关系》各新建了进度记录（修订号 1，位于首页）。用户此前在读书籍的修订号 250 未受影响。
- 未验证：修复版在真机上的最低可用内存、统计耗时和输入延迟；内存估算在真机上是否足够保守。
- 另见的既有风险（未改）：读者翻到超限章节时，阅读侧同样会完整解析该章后才报错，内存占用与本次相同，可能重启。

## 2026-10-06 · 全书页数、设置页补齐（已安装，待真机确认）

- 起因：用户对照设计稿后要求页脚显示全书页数和百分比（D16），并按顺序补齐设置页。
  实现前核对了 EPub-InkPlate、CrossMux、KOReader 和乐鑫官方接口，取舍见 [依赖审查](dependency-review.md)。
- 全书页数：`ReaderApp` 新增 `background_due()` / `background_step()` / `book_pages()`。
  固件主循环在无输入 1 秒后、无待呈现帧、无网络任务且未请求休眠时调用 `background_step()`，统计期间保持高性能档；
  桌面预览在事件等待超时时调用，脚本新增 `count` 命令。`SpineItem` 新增 `bytes`，用于统计完成前的百分比估算。
  分页缓存写入 `PicoPage/cache/pages/`。页脚、工具页眉和休眠画面改用全书位置。界面规则见 [UI 实现](ui-implementation.md) 第 12 节。
- 设置页：顶栏样式、首页当前值、存储与设备、输入与阅读、KOReader 同步只读页、返回路径；
  `FileStore` 新增 `card_space()`，`apply_settings` 可在全局层写入 `library.view`（`library.roots` 仍只读）。
  内置提示字体按新文案重新生成，由 474 个字符增至 504 个。
- 实际执行（仓库根目录）：
  - `cmake --build build/host --parallel 8`，`ctest --test-dir build/host`：**9/9** 通过。
  - `cmake --build build/sanitized --parallel 8`，`ctest --test-dir build/sanitized`：**9/9** 通过。
  - `.local/font-tools/bin/python tools/setup_fonts_subset.py --strings firmware/components/reader_app/app.cpp`：生成 504 个字符。
  - `sh tools/build_firmware.sh`：成功，日志 `.local/pages-settings-20261006/firmware-build.log`。
  - `python3 tools/check_handoff.py`：通过。
- 新增检查：`app_test` 覆盖分片统计不产生帧且不改变画布和已保存记录、页码随翻页增减、重启后直接采用缓存、
  改字号后重新统计且总页数变化、截断的缓存被忽略；设置各子页返回设置、K2 保持页面、书库默认视图写回且保留其他字段、
  设置文件无效时不改写。`store_test` 覆盖 `library.view` 的全局保存与注释保留、单书层拒绝、`library.roots` 只读、`card_space()`。
- host 实测：《吶喊》（公开样书）默认排版共 428 页，分页缓存 1,880 字节；截图见 `artifacts/pages-settings-20261006/`。
- 产物：`.local/pages-settings-20261006/pico_page.bin`，2,110,864 字节，
  SHA-256 `3dc46d3bf986271471577be7696b4e0a10d5e11405ee32ac3b6884e8462dc4fd`，与 `firmware/build/pico_page.bin` 一致。
- 烧录：用户确认（“烧录吧”）后仅写入 `0x10000`，`Hash of data verified`（`flash.log`）。写入前核对 USB 序列号与芯片 MAC
  均为 `…:18:B8`，只有这一台设备连接。写入前设备在正文，修订号 250、画布 CRC `b8362619`（`pre-status.log`）；
  重启后进入书库，TF 已挂载，首帧成功（`post-status.log`）。没有注入任何输入，未打开书籍，进度未变。
- 未验证（均需真机）：
  - 统计全书页数的实际耗时。每个新字符首次取字宽需读 TF 卡，此前 20,370 个字符的章节排版约 2.7 秒，大书可能需要数分钟；
    统计期间 CPU 保持高频，耗电未测。
  - 统计期间的输入延迟。读取一章是不可分的一片，大章节可能使一次点击晚数百毫秒才响应。
  - `esp_vfs_fat_info` 首次调用耗时。FSInfo 无效时 FatFs 会扫描整张 FAT 表，进入设置首页或存储页可能等待数秒。
  - 新顶栏和列表行在面板上的观感与热区；内置提示字体的新字形。
- 已知限制：页码可能在连续翻页时重复或跳过一个（见 UI 实现第 12 节）；“清理全部缓存”后当前书的页数保留在内存，
  下次开书重新统计；书籍和缓存大小的统计最多遍历 2000 项。
- 下一步：等待用户实际操作的反馈；据此测量统计耗时和输入延迟，再决定是否调整分片长度或把统计推迟到更长的空闲之后。

## 当前产物

- **当前已安装**：`.local/provisioning-20261006/pico_page.bin`，2,225,888 字节，
  SHA-256 `08a722f8122f7af09f6f5a8ac27772570836da5fb14345d9bd40769271d62479`，详见上方记录。

- 上一个已安装版本（版本 E）：`.local/pages-settings-20261006/pico_page-e.bin`，2,122,880 字节，
  SHA-256 `3bc0bb218ae28c20626da8f31309fd586ce71d302977590ce7f4bb536f8bcf1d`。

- 上一个已安装版本：`.local/perf-20261006/pico_page-perf-g.bin`，2,088,640 字节；
  SHA-256：`3b2a65383e2af00617955b23cdb6302bf362c2a89621c2a72d0b764d8488ec68`，同名 ELF 一并归档。
  仅写入 `ota_0`（`0x10000`），`Hash of data verified`。包含交互耗时修复、延后保存、独立触摸采样与连续翻页补执行，
  以及此前未烧录的离线界面与书籍管理改动。同目录其余 `pico_page-perf*.bin` 为已被取代的中间版本。

- 本批书籍管理与同步界面候选：`firmware/build/pico_page.bin`，2,077,072 字节，**仅构建，未安装**；
  SHA-256：`666ee8befc51c1a1476b79b97a50d71824df857fb07b317f9561ba292bb067e2`。
  归档 `.local/management-sync-20261006/pico_page.bin`。

- 上一批离线界面候选：`.local/offline-ui-20261006/pico_page.bin`，2,060,912 字节，**仅构建，未安装**；
  SHA-256：`59721dabc1dd82e13f09e64616c49c53b18c06557e4c53e29ffd02e21f454bb6`。
  归档 `.local/offline-ui-20261006/pico_page.bin`。

- 最后已安装的输入修复版本：`.local/page-input/pico_page-input-fix.bin`，2,030,048 字节，已安装到 OTA0；
  SHA-256：`ff3d30b61d30dbff5af4efa98d5154c651ed30aec18f7a0db6269b065ae96172`。
  归档 `.local/page-input/pico_page-input-fix.bin` 和同名 ELF。

- 上一批书库封面候选：2,029,008 字节；
  SHA-256：`269354fb7f7b3eee83e6ea4a8456a242a1ea180bfcf3a60ceb1081b391c685c9`。
  归档 `.local/library-covers/pico_page-library-covers.bin` 和同名 ELF。

- 桌面程序：`build/host/pico_page`。直接运行 `sh tools/run_host.sh`，使用同一原生 C++ 阅读核心。
- 上一批界面/K2 候选：`firmware/build/pico_page.bin`，2,026,000 字节；
  SHA-256：`efeeb682ca4007c43fbfeefb642108466e52ee135060805ad0895d2f04011d48`。
  归档 `.local/ui-restoration/pico_page-ui-k2.bin` 和同名 ELF，构建日志 `firmware-build.log`。
- 上一批 USB 候选归档：`.local/usb-storage-check/pico_page-usb-storage.bin`，2,010,352 字节；本地 3 MiB 应用构建配置。
  SHA-256：`377c714992ae30a72601f4eaea354149a9557c4f8d5b538a0bd94bdd47451a9d`。
  包含图片解码与 App 分页、资源导入、主循环门控及 411 字符内置提示字体；包含本轮崩溃、内存、字体与反馈修复。
- 演示卡：`.local/card`，原创 EPUB、公开鲁迅 EPUB、完整中文字体及许可证；实现截图：`artifacts/runtime`。
- 本地试用包：`.local/initial-build` 是此前的构建快照，不包含最新增量，也不包含设备烧录脚本。
- 截至 2026-10-06 的改动已提交到本地 main；未 push、未发布，仓库没有配置远端。
  仅写入实测 4 MiB `ota_0` 槽的应用，偏移 `0x10000`；原 bootloader、分区表、NVS、otadata 和校准数据未改。

## 2026-10-06 · 用户反馈：连点翻页偶尔没响应（已改并安装，用户已确认）

- 用户在延后保存版本上反馈：连续点击翻页时仍偶尔没响应。
- 证据（`.local/perf-20261006/physical-input-2.log`，约 30 次实体点击）：连点时下一次按下几乎都在
  帧完成后 12–17 毫秒被检测到，也就是恢复采样后的第一个样本。手指在上一页刷新期间已经按下，
  只有刷新结束时仍未抬起的点击才被看到；刷新期间按下并抬起的点击没有任何记录。
- 修改（决定 D14）：
  - 触摸由独立任务采样并带时间戳写入队列，应用任务被排版、刷屏或写卡阻塞时不再丢失；
    主循环改为等待队列（最长 20 毫秒），不再固定延时。
  - 采样任务由触摸中断线（GPIO43 下降沿）唤醒，接触前后 200 毫秒内每 5 毫秒采样，空闲时每 50 毫秒采样作为后备。
    持续 5 毫秒采样的版本使新页排版慢约两成（0.31–0.70 → 0.35–0.82 秒），因此没有采用；现版本排版和波形时间恢复。
  - 在上一帧完成前开始的点击标记为 `queued`：只有正文中紧接着翻页的翻页点击（左右区域或 K1/K3）会补执行，
    且不触发链接；K2、中部、页脚和其他页面上的此类点击一律丢弃，避免点到已被替换的画面。
    同一次刷新期间的多次点击只保留最后一次。
  - 休眠前后由互斥量阻止采样任务访问触摸控制器，唤醒后清空队列。
- 实际执行：host CTest **9/9**，sanitized CTest **9/9**；`sh tools/build_firmware.sh` 成功（`firmware-build-g.log`）。
  `app_test` 新增：开书后的排队点击被忽略；翻页后的排队翻页被执行且可回翻；排队的中部点击、K2、页脚点击和工具页上的点击被忽略。
  采样任务、中断和队列只在固件中编译，host 未覆盖。
- 烧录：用户确认后写入两次，均只写 `0x10000` 并校验摘要、核对序列号与 MAC。第一次为持续 5 毫秒采样的版本，
  测得排版变慢后改为中断唤醒并再次写入；第二次写入属于同一轮复测，未另行询问。
  当前安装 `.local/perf-20261006/pico_page-perf-g.bin`，2,088,640 字节，
  SHA-256 `3b2a65383e2af00617955b23cdb6302bf362c2a89621c2a72d0b764d8488ec68`。
- 设备复测（诊断输入，`serial-after-g.log`）：新页排版 0.31–0.70 秒、总计 0.81–1.20 秒；已缓存的上一页 0.59–0.62 秒；
  波形 376 / 1,363 毫秒与此前相同；日志无触摸读取错误、波形失败或显示错误。
  烧录前在读的书已重新打开，修订号 206、画布 CRC `6ac88c11` 与烧录前一致。《阿Ｑ正傳》回到测试前页面。
- 用户实际操作后确认：连点翻页的响应可以接受（“试了一下，不错”）。
  随后读取的设备日志出现 `Touch interrupt line is active`，触摸中断线实际触发；
  日志缓冲只保留了 4 次点击，均为即时派发（`waited=0`），未能从中看到补执行的点击，也没有读取或显示错误
  （`.local/perf-20261006/physical-input-3.log`）。
- 用户随后反馈已测试休眠再唤醒后的触摸，未报告问题。此后诊断 `status` 正常应答，设备在正文，
  帧计数继续增加、最近一帧成功；休眠前后的串口日志没有保留，未能从日志核对休眠与唤醒过程。
- 未验证：空闲电流；补执行路径和休眠唤醒后的触摸只有用户的实际操作确认，没有日志样本。

## 2026-10-06 · 用户反馈：翻页偶尔没反应或明显变慢（已改并安装）

- 用户在已安装的交互耗时修复版本上实际阅读后反馈：点击翻页有时没反应，有时耗时明显变长。
- 证据：设备串口缓冲中保留的实体触摸日志，10 次点击，其中 5 次连续翻页
  （`.local/perf-20261006/physical-input-20261006.log`，设备时间戳，单位毫秒）。
- 没反应的主要原因——页面出现后的输入盲区：正文刷完后主循环仍在同步保存进度，这期间不读触摸。
  用户在读的一本书每次保存 195–202 毫秒（书库目录靠后、文件名长，路径查找更慢；鲁迅样书约 110 毫秒）。
  日志中连续三次翻页的下一次按下分别在保存结束后 87、87、107 毫秒被检测到，即页面出现后约 280–300 毫秒，
  与人看到新页后的反应时间相当。用户的点击持续 21–81 毫秒；稍早一点落在保存期间并已抬起的点击不会被看到。
- 次要原因——采样间隔：主循环每 20 毫秒读一次触摸，10 次点击中有 3 次只被采到一个样本（约 21 毫秒）。
  更短的轻点可能落在两次采样之间。板级参考固件的循环间隔为 5 毫秒（`main/app/app_loop.c` 的 `LOOP_TICK_MS`）。
- 按设计不响应的情况：排版与刷屏期间（约 0.9–1.3 秒）完成的点击不排队；
  点在正文底部 88 像素（页脚）切换页脚内容而不翻页；按键区上方 y 1216–1300 的空白带和 K3 右侧 x ≥ 480 无动作。
  日志中未出现多点或跨区取消。
- 变慢的原因：这 5 次翻页的排版准备为 302–735 毫秒（新字形多的页更慢），加呈现约 560 毫秒；
  均衡档每 8 页一次全刷，波形由 0.48 秒变为 1.36 秒，该页总计约多 0.9 秒；跨章需读取并解析下一章；
  停留超过 8 秒后的第一页多约 85 毫秒面板上电。没反应后重点一次也会被感知为变慢。
- 用户决定采用延后保存。已实现：
  - 主循环间隔由 20 毫秒改为 5 毫秒（已被上一条的独立采样任务取代）。
  - 翻页后不再在帧完成时同步保存。主循环在 2 秒无输入后调用 `save_progress()`；
    `ReaderApp` 新增 `save_due()` / `save_progress()`，取代 `retry_save()`。
  - 打开工具前、错误页显示前立即保存，使页眉“尚未保存”和错误页“重试保存”只反映真实的保存失败；
    返回书库、开书、休眠、USB、上传和书籍管理原本就先保存，未改。
  - 保存失败后不自动重试，等待用户在错误页或设置页重试；新页面成功呈现后恢复自动保存。
  - 桌面预览在每帧后立即保存，行为与原先一致。
- 实际执行：`cmake --build build/host -j 8` 后 CTest **9/9**；`build/sanitized` 同样 **9/9**；
  `sh tools/build_firmware.sh` 成功（`.local/perf-20261006/firmware-build-e.log`）。
  固件 2,085,024 字节，SHA-256 `88ee4586d81102c60ec6f75d05e5b0f276f991076fad8d51b10ce719cd627054`，
  归档 `.local/perf-20261006/pico_page-perf-e.bin` 和同名 ELF。
- 新增检查（`app_test`）：连续翻页不产生中间保存；空闲保存提交最后一页且修订号连续；
  打开工具和休眠前立即保存。既有保存失败、休眠、USB 用例经调整后的驱动继续通过。
- 烧录：用户确认后仅写入 `0x10000`，`Hash of data verified`；写入前核对 USB 序列号与芯片 MAC（`flash.log`）。
- 设备复测（诊断输入，`.local/perf-20261006/serial-after-e.log`）：
  - 连续 4 次下一页，每帧完成后的保存耗时为 0，修订号保持 342；停止输入 3.5 秒后修订号变为 346，只提交了最后一页。
  - 新页总计 0.83–1.11 秒（上一版 0.92–1.30），已缓存的上一页 0.56–0.57 秒（上一版 0.71–0.74）。
  - 翻页后立即打开工具：先提交再显示，修订号随即更新。
  - 从“继续阅读”打开 12.5 MB 的书：0.06 秒后出状态帧，总计 6.27 秒，恢复到烧录前的页面。
  - 日志中没有保存失败或显示错误。
- 测试副作用：《阿Ｑ正傳》修订号 342 → 352，回到测试前页面。复测中误开了一次另一本用户书籍（修订号 32 未变），
  随后重新打开烧录前在读的书，修订号 40、画布 CRC `94beb2b5` 与烧录前一致。
- 已知残留：2 秒后的那次保存仍会阻塞约 0.1–0.2 秒，恰好在此时完成的短点击仍会丢失；
  翻页后 2 秒内断电或复位会回到上一次已保存的位置；排版与刷屏期间的点击仍不排队。
- 未验证：实体点击是否不再丢失（需用户实际操作）；休眠前保存（需按电源键）；5 毫秒间隔对空闲电流的影响。盲区结论来自一段 10 次点击的日志，
  样本少，未直接观察到被丢弃的点击（丢弃的点击不产生日志）。

## 2026-10-06 · 交互耗时修复（已安装并复测）

- 实现前核对了 CrossMux、CrossPoint、freeink-sdk、KOReader 封面缓存和乐鑫官方资料，
  来源与取舍见 [依赖审查](dependency-review.md)。没有新增依赖或复制上游代码。
- 字宽：`Font::advance` 对静态 SFNT 字体改用 `FT_Get_Advance(FT_LOAD_NO_SCALE)` 取设计字宽后缩放取整，
  不再为取字宽加载整个字形；可变字体或取值失败时仍走原路径。
  文楷、思源黑体和内置提示字体在 20–76 像素共 2,229,752 次比对无差异（`.local/perf-20261006/advance-compare.log`）；
  两本鲁迅 EPUB × 三档字号的 2,060 页分页边界与像素摘要和既有基准逐行相同（`pages-fast-advance.log`）。
  其他字体若带设备度量表（hdmx），字宽可能与完整加载相差 1 像素；排版与绘制仍使用同一字宽。
- 封面缓存：书库缩略图、详情封面和休眠封面解码一次后写入 `PicoPage/cache/covers/`，
  按原书路径、大小和修改时间校验；没有可用封面的结果也记录，分配失败不记录。
  `FileStore` 新增 `load_cache` / `save_cache`；格式和限制见 [存储](storage.md)。
- 书库卡片先显示书名占位和“正在生成封面…”，随后解码并再刷新一次；已缓存时没有中间帧。
- 回翻：从章首重排时记录途中全部页起点，之后的回翻直接使用；跨章回翻沿用同一路径。
  重排范围超过 10,000 个字符时先显示“正在排版…”。翻页结果与改前相同。
- 开书：书库、继续阅读和详情入口先显示“正在打开书籍…”再开书；所选字体未变化时不再重新打开字体，
  保留字形与字宽缓存。诊断 `open` 命令仍为同步。整本 SHA-256 身份计算未改，待用户决定。
- 图片缩放：每像素 4 次 64 位除法改为预先计算的坐标表，5 张封面 × 6 种尺寸输出逐字节相同。
  真机上 1600×2400 PNG 图片页由 8.87 秒变为 8.43 秒，收益很小：耗时在 PNG 解码器本身。
- `read_file` 改为按文件大小一次读入。等待反馈的界面规则见 [UI 实现](ui-implementation.md) 第 11 节。
- **未采用**：`CONFIG_FATFS_VFS_FSTAT_BLKSIZE=4096`。官方指南建议为 stdio 读取增大缓冲，但真机对照显示
  新页准备时间由 0.41–1.01 秒升到 0.67–1.68 秒（字形是随机小块读取，每次多读了 4 KiB），已恢复默认值。
- 更正：上一条排查记录把《吶喊》的跨章回翻写成“9.8 万字的章节”。98,722 是全书字符数，
  该章实际为 20,370 个字符、49 页。
- 实际执行（仓库根目录）：`cmake --build build/host -j 8` 后 CTest **9/9**；
  `cmake --build build/sanitized -j 8` 后 CTest **9/9**，无 ASan/UBSan 报告；
  `sh tools/build_firmware.sh` 成功（`.local/perf-20261006/firmware-build-c.log`）；
  `python3 tools/check_handoff.py` 通过；`git diff --check` 通过。
- 新增检查（`app_test`）：封面占位帧后出现封面；缓存缩略图跨重启复用、无效条目重建；
  点击开书先出状态帧且此时不产生进度；回翻进入长章节先出状态帧并保持已确认位置，
  随后的回翻不再重排，前后翻回到原位置。新增夹具 `long-chapter.epub`。
- 烧录：用户授权后共写入三次，均只写 `0x10000` 并校验摘要；每次写入前核对 USB 序列号与芯片 MAC
  均为 `…:18:B8`（`.local/perf-20261006/flash.log`）。第一版含 4096 字节缓冲，第二版仅把它改回默认用于对照，
  第三版为当前安装版本。第二、三次写入属于同一轮复测，未另行询问。
- 设备复测（诊断输入，`.local/perf-20261006/serial-after*.log`；“总计”含呈现与保存）：

| 操作 | 改前总计（秒） | 现在总计（秒） | 说明 |
|---|---|---|---|
| 回到浏览过的书库卡片屏 | 5.8–18.9 | 0.76–0.84 | 封面来自 TF 缓存 |
| 重启后进入已生成封面的卡片屏 | 2.8–18.9 | 0.99–1.28 | 书名字形首次绘制占 0.4–0.7 |
| 首次生成一屏封面 | 2.8–18.9，无反馈 | 4.4–21.1，约 2.6 秒内先出占位 | 每本书只发生一次；含两张大 PNG 的一屏最慢 |
| 下一页，新页 | 1.0–1.7 | 0.92–1.30 | 准备 0.41–1.05 → 0.30–0.69 |
| 上一页，已缓存 | 0.70–0.74 | 0.71–0.74 | 未变 |
| 跨章回翻到 20,370 字符的章节 | 9.78 | 2.66 | 约 1.5 秒时出现状态行 |
| 长章节深处首次回翻（无页起点） | 未测 | 2.83，之后 0.98 | 约 0.7 秒时出现状态行 |
| 开书，0.1–1.4 MB（诊断同步命令） | 1.4–3.1 | 1.2–2.2 | |
| 点击封面开书，1.14 MB | 2.11，无反馈 | 2.55 | 约 0.65 秒时出现状态行 |
| 开书，14.3 MB | 未测 | 5.92 | 继续阅读入口约 0.8 秒时出现状态行 |
| 排版预览，新字号 / 换字体 | 1.3–1.6 / 2.0 | 0.88–0.98 / 1.0–1.47 | |
| 翻到 1600×2400 PNG 图片页 | 9.58 | 9.14 | 基本未变，无状态行 |
| 书籍详情，1.14 MB | 未安装 | 1.97，封面已缓存时 1.12 | |

- 测试副作用：《阿Ｑ正傳》修订号 306 → 342，《吶喊》120 → 161，均回到测试前页面；
  另有四本用户书籍被打开过，修订号未变。
  烧录前设备停在用户在读的书（修订号 7），复测后已重新打开该书，修订号仍为 7。
  TF 卡新增 `PicoPage/cache/covers/` 下的封面缓存文件。未改设置。
- 未验证：实体触摸端到端耗时；状态行和占位封面在面板上的观感；休眠封面缓存（需按电源键）；
  TF 满卡或掉电时写封面缓存的行为；内存长期占用。
- 未处理：大幅面 PNG 图片页仍需约 9 秒且无状态行；开书的整本 SHA-256；翻页后保存阻塞约 0.11 秒；
  刷屏期间短触摸丢失；新字形首次绘制（位图）仍需从 TF 读取；详情/管理页对整本书的重复摘要与 CRC。
- 下一步：请用户实际操作确认体感；再决定 PNG 图片页（缓存或状态行）与开书身份计算的处理方式。

## 2026-10-06 · 交互耗时排查（只测量，未改代码）

- 起因：用户反馈部分点击后等待很久才切换，且没有加载态。本批通读应用、文档、存储、图形和平台层的
  同步路径，并在已安装的输入修复版本上用诊断输入逐项计时。未改固件源码，未烧录，未改设置。
- 方法：`.local/perf-20261006/probe.py` 经诊断串口发送 `tap`/`key`/`open`。“准备”为命令到 `OK` 的时间
  （`app.input()` 内的同步工作），“总计”到 `status pending=0`（含呈现与保存）。不含手指按下到释放的时间，
  不代表实体触摸端到端耗时。原始记录 `.local/perf-20261006/serial.log`，读卡速率 `readrate.log`。
- 下限：任何换屏 = 画布转换 75 ms + 波形 480 ms（快速档 376 ms）+ 主循环约 50 ms，合计约 0.61 秒；全刷 1,363 ms。

| 操作 | 准备（秒） | 总计（秒） |
|---|---|---|
| 书库卡片翻屏，封面未缓存 | 2.2–5.8 | 2.8–6.5 |
| 回到书库第 1 屏，封面缓存已被淘汰（含两张 1600×2400 PNG 封面） | 18.3 | 18.9 |
| 书库卡片翻屏，封面已缓存 | 0.15 | 0.77 |
| 列表翻屏 / 排序 / 进入文件夹，首次 | 0.69–0.77 | 1.3–1.5 |
| 开书，0.1–1.4 MB 的六本 | 0.65–2.26 | 1.4–3.1 |
| 下一页，新页（含保存 0.11） | 0.41–1.05 | 1.0–1.7 |
| 上一页，已缓存 | 0.09–0.10 | 0.70–0.74 |
| 翻到图片页（1600×2400 PNG） | 8.87 | 9.58 |
| 跨章回翻到 20,370 字符的章节（原记为 9.8 万字，见上方更正） | 9.04 | 9.78 |
| 打开 / 关闭工具 | 0.15 / 0.11 | 0.76 / 0.71 |
| 目录，首次打开 | 0.50–1.41 | 1.1–2.1 |
| 排版预览，新字号 / 换字体 | 0.71–0.91 / 1.40–1.46 | 1.3–1.6 / 2.0 |
| 返回书库（全刷） | 0.16 | 1.65 |

- TF 卡整文件顺序读约 3.0 MiB/s（只读 `file` 命令，14.3 MB 用 4.51 秒）。开书对整本 EPUB 计算 SHA-256，
  14 MB 的书仅读取就需约 4.6 秒；未直接测量大书开书，因为打开未读过的书会新建进度记录。
- 代码定位（工作目录当前源码）：
  1. `image.cpp` 的 `Raster::sample` 对每个源像素做 4 次 64 位除法；1600×2400 的 PNG 约 8.9 秒。
     影响正文图片页、书库封面、详情封面和休眠封面。
  2. 书库封面缩略图只存内存且最多 12 张；浏览 3 屏以上或重启后重新解码。
  3. 跨章回翻同步排完整个上一章；耗时主要来自每个新字符首次取字宽时的 `FT_Load_Char`。
     无缓存页起点的回翻每次从章首重排，且不记录途中页起点。
  4. 开书每次整本计算身份摘要，并重新打开正文字体（清空字形与字宽缓存）；全过程无加载态。
  5. 未烧录的详情/管理页：进入详情整本摘要 + 再次打开取封面；每次重画详情都重新解码封面；
     确认页再做一次整本摘要和一次整本 CRC，执行时再做一次整本 CRC。休眠封面在休眠前同步解码。
  6. 新字形首次使用约 7–10 ms（字宽和位图各加载一次）；TF 读取与 FreeType 计算的占比未测。
  7. 每次翻页后的进度保存阻塞 105–200 ms；呈现和保存期间不读触摸，期间完成的短触摸丢失。
  8. `sync_badge()` 每次画阅读页眉都读 `sync.toml`；`scan_library()` 每次重读设置并逐项 `stat`。影响为几十毫秒。
- 测试副作用：《阿Ｑ正傳》修订号 294 → 306，《吶喊》102 → 120，均回到测试前页面（画布 CRC 相同）。
  用户在读书籍修订号 56、页面 CRC `87cfaf6a` 不变；最近阅读已恢复为该书；设备停在测试前的书库画面（CRC `26e11690`）。
- 未验证：实体触摸端到端耗时；本批及上一批未烧录页面的真机耗时；大书开书；字形耗时的 TF/CPU 拆分。
- 下一步：待用户确定修复顺序。建议先做 1–4 并为超过约 1 秒的操作补加载反馈，均可先在 host 验证输出不变。

## 2026-10-06 · 书籍管理、同步及错误恢复（未烧录）

- 按用户新增工作要求补查乐鑫官方 SDMMC/FatFs 示例、KOReader 同步插件与官方服务器，
  沿用已审查的 FileStore 和精确映射路径，没有新增依赖或复制上游应用代码；来源见 dependency-review。
- 详情页增加本书内存缓存清理、清除本地进度和删除原书。后两者先显示独立确认页，
  K2 只全刷。文件状态变化时拒绝执行；删除只移除原 EPUB，保留伴随目录的记录与设置。
- 清除进度以 schema 2 清除标记先提交备份、再提交主文件；主文件缺失/损坏不会复活旧进度。
  当前书籍先结束会话并取消异步工作。重开从头显示成功后才重新保存；旧固件会拒绝该清除标记，
  不会将其视作普通坏文件并恢复旧位置。部分提交失败仍报告失败，必须重读后再操作。
- 同步按 S06 恢复正文、工具栏、三栏状态和底部操作；远端栏明确是服务器记录，不是设备在线状态。
  S07 冲突页提供继续本机、使用精确远端位置和单独确认覆盖上传；无近似跳转或自动上传。
  已确认上传绑定确切记录和账户；在途取消/未确认回包保留待核实状态，旧请求结果不能确认新记录。
- 错误提示在正文可读时重绘最后确认位置；重绘失败或 TF 移除时使用内置提示页。
  重试保存只有确认成功后恢复正文，失败保留错误状态。所有新增页面保留 K2 语义。
- 实际验证（仓库根目录）：`cmake --build build/host -j 8` 后 CTest **9/9**；
  `cmake --build build/sanitized -j 8` 后 CTest **9/9**，无 ASan/UBSan 报告；
  `sh tools/build_firmware.sh` 成功；沙箱内依赖检查的 `sysctl` 权限失败后使用获准的沙箱外构建。
  `python3 tools/check_handoff.py` **300 项通过**；`git diff --check` 通过。
- 延伸现有 store/store_sync/app 检查，覆盖外部文件变化、清除备份恢复、写同步失败、
  删除保留用户记录、取消确认、K2、重新开书、取消上传后的旧回包、登录失效和明确上传确认。
  日志和截图驱动归档 `.local/management-sync-20261006/`。
- 截图 `artifacts/management-sync-20261006/` 来自当前 C++ 应用、合成 EPUB 和模拟服务回包；
  目视检查同步空闲/检查中/冲突/无记录/登录失效/已上传、详情、清除/删除确认和错误恢复。
  不属于真机或实际服务器验收。本批未提交、未推送、未烧录；原始设计文件保持不变。
- 剩余：详情内目录入口、完整网络/账号输入界面；真机灰阶、热区、TF 掉电/满卡和 Wi-Fi 接续。
  下一步先按截图与实际设备反馈继续收敛；用户仍要求优先离线阅读，网络配置尚未启用。

## 2026-10-06 · 离线界面补齐（未烧录）

- 刷新 S08 恢复上方正文、下方单选行与“当前”标签；快速模式经屏幕确认后生效。
  新增均衡/快速各自的自动全刷间隔，范围 1–30 页；原默认仍为 8/5 页，清晰模式每页全刷。
  取消不写配置；K2 在确认和间隔草稿页只全刷，保持像素、草稿和已保存位置。
- 休眠 S11 新增原书封面、无封面文字回退、保留正文和简洁屏保。封面按比例居中裁切到
  684 × 1040，解码临时像素不超过目标框；不支持或损坏时显示文字封面。
  保存确认后才绘制“此处已保存”，同步状态来自休眠前的实际会话；无书时不声称已保存。
  唤醒提示仍为电源键，没有增加电容键唤醒或改变板级睡眠。
- 书库列表右侧进入单本详情；卡片封面打开书，底部标题/详情区域进入详情（88 px 热区）。
  详情按需校验单本 EPUB 身份，显示书名、作者、封面、已验证的保存章节、原文件占用和路径；
  不打开阅读会话，不更新最近阅读或进度。无封面不留空白封面框；长标题最多四行。
  删书、清除进度、单本缓存管理及详情内目录入口仍未实现，未显示可执行的假按钮。
  同时修正封面缩略图读取相邻两像素的低/高半字节顺序。
- 设置 S10 恢复六组入口；新增电源与休眠的显式保存/取消、输入说明及当前书排版入口、
  存储与设备说明和实际缓存清理。未保存的休眠选项为虚线“待保存”。
  存储容量尚未接入统计，显示未知；网络与同步继续沿用已有能力，未新增配网或无线传输。
- 配置仍使用原 FileStore 提交、注释保留和外部改动检查。新增
  `display.balanced_full_refresh_pages`、`display.fast_full_refresh_pages`、`power.sleep_screen`；
  电源字段仅全局可写，间隔允许本书覆盖。“使用阅读默认”会移除本书的刷新间隔覆盖。
  修改刷新和电源设置不顺带应用外部编辑的正文排版。旧固件不识别新增字段，回退前需保存配置副本。
- 实际验证：`cmake --build build/host -j 8` 与 CTest 9/9；`build/sanitized` 的 ASan/UBSan 9/9。
  扩展既有 app/store/graphics 检查：确认/取消、K2、详情无会话副作用、保存失败拒绝睡眠、
  休眠唤醒保留进度、失败目标不被休眠/唤醒采用、清缓存保留原书与记录、TOML 冲突、
  间隔边界及 PNG/JPEG 中心裁切。
  未新增测试框架；原 SDL/OpenSSL 目标版本链接警告仍存在。
- `sh tools/build_firmware.sh` 首次受沙箱 sysctl 限制；在授权环境中最终编译成功。
  内置字体子集更新为 474 字符。截图为 684 × 1216 当前原生 host 输出，位于
  `artifacts/offline-ui-20261006/`；日志与候选在 `.local/offline-ui-20261006/`。
  使用临时演示卡副本，没有更改用户 TF 卡、设备或原始设计稿。
- 未验证：本批真机显示/触摸/睡眠、封面解码耗时和峰值、可配置刷新间隔下的残影。
  下一步：先目视检查本批截图；后续补详情管理、完整同步/冲突界面与错误恢复状态。
  本批未提交、推送、发布或烧录。

## 2026-10-06 · 偶发翻页响应诊断

- 用户反馈翻页偶发等待很久或需要点两次。代码确认：实体按下即进入同步排版/刷新/保存，
  主循环此时不读取触摸；释放与下一次按下若均发生在阻塞期间，可被合并为同一持续接触。
  实体动作改为完整释放后派发一次，保留初始坐标；取消不执行，长按不重复。
  按下与释放间若提交过其他帧，取消该手势，防止异步页面变化后输入穿透。
- 另发现每帧主动诊断通知使用最多 1000 ms 的发送等待。无主机读取或发送缓冲区满时，
  可额外阻塞阅读。`FRAME`/`READY` 改为零等待；明确命令回复仍保留原有可靠发送策略。
- 增加实体输入阶段、动作准备及刷新/保存分段耗时日志，区分检测、排版、波形与存储。
  不改分区、书籍、网络设置，不将串口注入等同于实体输入验收。
- 现有 core 测试覆盖释放前不派发、保持初始目标、重复释放、错配序列、取消、页面代次变化和下一次独立触摸。
  host 与 ASan/UBSan 各 9/9；`sh tools/build_firmware.sh` 通过。
  记录目录 `.local/page-input/`。实体传感器漏检及物理双击改善仍需真实操作确认。
- 仅烧录 `0x10000`，摘要验证成功；重启后恢复修订号 23、正文 CRC32 `31d9a958`。
  三组串口下一页/上一页往返通过，最终仍为同一页，正常翻页使修订号增至 29；K2 不增加修订号。
  已缓存正文样本的命令往返约 0.90–0.94 秒，首次下一页 1.96 秒；这些是应用注入耗时，
  不等于实体触摸端到端耗时。日志显示普通波形约 481 ms、进度保存约 197–199 ms，
  K2 波形约 1363 ms。未取消持久化或放宽保存确认规则。
- 最终版启动、原页恢复与 K2 再次验证通过，修订号 29、CRC32 `31d9a958` 保持一致；
  设备停在正文，串口客户端已退出。55 秒实体日志采集未收到输入事件，不作为硬件检测通过。
  最终版仍需用户真实轻点反馈。

## 2026-10-06 · 书库封面与返回全刷

- 从阅读工具/目录返回书库明确请求 `FullClean`；K2 继续全刷当前画布。
- 卡片读取 EPUB 3/2 声明的 PNG/JPEG 封面及简单封面页，无声明时仅尝试首章纯图片页。
  无封面、格式不支持或损坏时显示文字封面。仅解码当前屏最多六本，不读取整书计算身份，
  不修改正文文档实例、会话或已保存进度。最多保留两屏内存缩略图（181,056 字节像素），
  根据文件大小/修改时间失效，TF 卸载时清空；未引入封面数据库或磁盘缓存。
- `cmake --build build/host -j 8`、CTest：9/9；`build/sanitized` 同样 9/9。
  既有测试增加 EPUB 2/3/guide、首章图片/SVG 封面、非法路径与正文插图回退，
  卡片像素、K2 保持像素/位置及返回书库全刷验证。日志在 `.local/library-covers/`。
- `sh tools/build_firmware.sh` 通过；仅写入 `0x10000`，esptool 报告摘要验证成功。
  启动后恢复阅读修订号 23、CRC32 `31d9a958`，与烧录前一致；返回书库 `clean=1`。
- 最终缓存版本真机：重复切入卡片与正文恢复通过；从正文返回已缓存卡片屏约 1.65 秒，
  包含 1.363 秒全刷；K2 保持画布和修订号。首屏未缓存六本的读取/解码约 20 秒，
  仍是当前等待限制。测试日志 `.local/library-covers/device-cached.log`。
  最终设备卡片画布抓取校验通过，已检查真实封面及文字回退；图像仅存 `.local/library-covers/device-grid-cached.png`。
- 桌面实际画布 `artifacts/design-audit/after-library-covers.png`：公开鲁迅 EPUB 原封面、
  原创图片夹具及无封面文字回退并列检查通过。物理面板的残影与清晰度仍需用户目视确认。

## 2026-10-06 · 原稿还原与全局 K2（本轮）

- 用户更新：所有可交互页面 K2 只全刷当前画布，不关闭面板、取消草稿、确认同步或退出 USB。
  刷新忙碌时重复输入丢弃；硬件睡眠依旧由 PMU 电源键唤醒，未增加电容键唤醒能力。
- 工具页恢复图标、粗体章节标题、同步真实状态、刷新档位；目录恢复标签导航、当前位置标签、
  子级展开/收起、长标题最多两行及显式翻屏按钮。目录进入时定位到当前条目所在屏。
- 排版恢复当前/预览区分和禁用“已应用”；新增明确“取消预览”，保留“收起”取消入口。
  K2 在预览中保留草稿与原图；应用、设为默认及文件冲突保护仍沿用原存储流程。
- 书库恢复列表/文字卡片切换、书名升降序、两行标题、近期阅读区域及有边界状态的翻屏条。
  切换和排序仅影响本次运行，不修改用户 TOML。列表仍以文件名为来源，未扩展全库元数据索引、
  原书封面缩略图或详情页；不把未知进度填成“未读”或伪造百分比。
- 正文页脚显示按当前章文本位置计算的“本章约 N%”，触摸可切换明确标注的章节序号；右侧接真实 PMU 电量。
  未计算完整章页数时不伪造“本章 N/M”；桌面没有电量数据时显示未知。
- FreeType 增加上游 `ftsynth.c` 编译与 UI 合成粗体，正文不改字重；内置提示字体为 427 字符。
- 本轮 host 和 ASan/UBSan 各 9/9 通过；在既有测试中覆盖 K2 保留页面/像素/进度、重复输入、
  排版草稿、网络请求、确认页和 TF 已卸载的 USB 页，以及目录展开/跳转与字体粗体缓存隔离。
  日志：`.local/ui-restoration/host-test.log`、`sanitized-test.log`。
- 684 × 1216 桌面实际画布位于 `artifacts/design-audit/after-*.png`；已逐图检查工具、目录、排版、
  列表和卡片。长标题展开后按 K2 的 BMP 逐字节一致。原始设计文件未修改。
- 用户确认“烧录吧”后，仅向 `0x10000` 写入 2,026,000 字节，`Hash of data verified`；
  日志 `.local/ui-restoration/install.log`。设备已成功启动并挂载 TF；正文、工具、排版、目录、书库、刷新、设置、导入共 8 类页面
  K2 诊断检查通过，均全刷且不改变页面、CRC 和保存修订号 7。排版草稿保留及显式取消恢复通过。
  未修改 bootloader、分区表、NVS、otadata、OTA1 或工厂校准；未提交或推送。
- 最终诊断脚本完整通过，设备停回原正文：`saved=1`、修订号 7、CRC32 `ca8b945a`。
  工具页设备画布已导出并检查，位于忽略目录 `.local/ui-restoration/device-tools.png`，串口客户端已退出。
- 收尾 `python3 tools/check_handoff.py` 通过 298 项，`git diff --check` 通过。主机仍有原环境 OpenSSL/SDL
  目标系统版本链接警告，构建和测试成功；固件构建无编译错误。

## 2026-10-06 · USB 文件传输候选

- 新增设置开关，默认关闭且不持久化；正常模式保留 USB Serial/JTAG。
- 进入前保存进度、关闭 EPUB/字体、检查 FatFs 卸载，再将原始 SDMMC 交给 TinyUSB MSC；退出确认后停止 I/O 并重启。
- 传输期间阻止阅读、同步、诊断、自动休眠及应用卡扫描；电脑安全弹出仍是用户操作前提。
- `cmake --build build/host -j 8` 与 CTest、sanitized 构建与 CTest：各 9/9 通过。日志 `.local/usb-storage-check/host.log`、`sanitized.log`。
- `sh tools/build_firmware.sh`：成功；`firmware-final-build.log`。仅写入已核实的 `ota_0`，烧录 `Hash of data verified`；`install.log`。
- 真机正常模式启动、TF 挂载、设置进入已验证；macOS 出现 `Pico Page TF Card` 并挂载 31.3 GB FAT32 卷。
- 电脑端 1 MiB 写入、落盘、重命名与 SHA-256 读回一致；首次访问等待 macOS 可移动宗卷授权，不纳入性能结果。目录整理完成：Books 19 本、Articles 52 篇，7 个 sidecar 随书保留，21 个字体保持原位；71 个目标文件 SHA-256 与原始导出一致。70 个文件移动，1 个含韩文组合字符的文件以短名复制，旧条目仍保留在 Books/Kindle。macOS 安全弹出成功；用户已执行退出重启；串口与 TF 恢复、迁移 EPUB 的 CRC32、进度修订号 3 和 K2 全刷保持进度均已验证。尚未再次执行完整烧录。
- 操作、所有权与边界详见 [USB 文件传输](usb-storage-runtime.md)。未提交、推送或发布；私人资源与设备日志仍仅在忽略目录和 TF 卡。

## 2026-10-06 · 官方 MSC 示例复核与文件名清理

- 已对照 ESP-IDF 6.1 固定提交的 `tusb_msc` 示例，并检查 esp_tinyusb 2.4.0 源码；官方 `auto_mount_off` 可关闭回挂，已修正文档此前不完整的理由。
- 保留同步块回调的原因是便利层延后写失败只记录日志；具体来源与差异见 [USB 运行记录](usb-storage-runtime.md)。本轮未修改固件代码或重复烧录。
- 71 个 EPUB 文件名清理计划已检查无重名，去掉导入序号与 Z-Library 标识；保留书名、作者与扩展名；71 个文件已重命名并逐个核对 SHA-256，7 个进度目录及最近阅读路径同步更新。一个此前无法按名称访问的旧副本仍留在 Books/Kindle，其正常使用副本已命名为 Books/素食者.epub。
- 重新挂载后，1 MiB 专用测试文件的 SHA-256 仍一致。私有记录：`.local/usb-storage-check/rename-results.jsonl`、`rename.log`、`after-usb.log`。

## 已接通的实际路径

- 原 EPUB → 有界 ZIP/XML → 当前章源位置 → Unicode 完整行分页 / PNG、JPEG 图片独立页 → 灰阶画布。
- 书库/文件夹、最近阅读、翻页、跨章、目录、内部链接及返回、工具、排版预览/应用/取消、刷新模式。
- 显示成功后保存；重开恢复；K2 立即全刷且锚点/修订号不变；显示失败恢复已确认位置。
- TOML 全局/单书层保留注释写回、外部编辑冲突保护、恢复默认、进度备份与失败重试。
- Read Pico 显示、工厂 VCOM 读取、触摸按键、TF、PMU 浅睡与独立 Wi-Fi/HTTPS 工作任务。
- 手动检查远端候选、精确映射后确认跳转、显式确认上传已保存版本；取消、旧回包、账户更换隔离。
- 缺完整字库、损坏记录、配置错误、映射不支持和网络失败均不以成功占位替代。
- 受限 USB 资源导入已实测完成：两套中文字体、许可证、《阿Ｑ正傳》和《吶喊》均读回 CRC 校验通过。

## 累计验证记录

工作目录：`/Users/wulu/workspace/pico-page`。

| 命令 / 操作 | 结果和证据 |
|---|---|
| `cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug`；`cmake --build build/host -j 8` | 成功；最新增量 `.local/host-current.log` |
| `ctest --test-dir build/host --output-on-failure` | 9/9 通过，含真实 app 开书、呈现失败、排版和异步同步回包回归 |
| `cmake --build build/sanitized -j 8`；`ctest --test-dir build/sanitized --output-on-failure` | ASan/UBSan 9/9 通过，无运行时错误；配置见 environment |
| `python3 tools/check_koreader_mapping.py` | 真实固定 CREngine：两个 DOM 版本 × 三档字号，每组 2,018 个精确源位置往返；`.local/koreader-reference-check.log` |
| 实际 CREngine 加载原始鲁迅 EPUB，两个 DOM 版本 × 三档字号 | 《阿Ｑ正傳》每组 45,172 个文本标量、857 个引擎书签；《吶喊》每组 98,722 个文本标量、1,875 个引擎书签；全部成功，图片元素未纳入精确同步位置范围 |
| 官方完整 KOReader 客户端经本地测试服务联测 | 两本原 EPUB × 三档字号，共六组双向接续和实际客户端重启恢复通过；详见 [同步记录](sync-runtime.md) |
| 原版鲁迅 EPUB 图片与整书分页 | 两本 PNG 封面实际解码、目检通过；host 与 ASan/UBSan 的三档字号逐页检查通过，见 [图片实现](image-runtime.md) |
| `sh tools/build_firmware.sh` | 最新完整构建成功，包含图片、App、导入与主循环门控；`.local/firmware-usb-protocol.log` |
| 仅写入 `ota_0` 应用区域 | `0x10000` 写入 1,944,560 字节，`Hash of data verified`；`.local/device/install/usb-protocol-flash.log` |
| 首版真机启动 | PMU、工厂 VCOM 读取、SD、PSRAM、书库全刷完成已确认；不等于物理目视和输入验收 |
| host `--headless --script .local/smoke.txt` | 真实 EPUB 打开/翻页/K2/工具/排版取消/目录/睡眠恢复；K2 前后保存修订号均为 5 |
| 检视 684 × 1216 实际渲染 PNG | 正文完整行；排版采用原稿结构；工具栏遮挡处移除半行残片，关闭后恢复原页 |
| SDL 窗口启动 | 沙箱内无法枚举显示设备；获准在本地 UI 环境启动后程序持续运行。CUA 无法绑定无 bundle 的命令行窗口，未声称其交互已由 CUA 验收 |
| `python3 tools/check_handoff.py` | 284 项通过；49 个原设计文件、36 张原 PNG、三份历史快照保持原样 |
| `git diff --check` | 通过 |

## 明确未完成 / 未验证

- 真机物理目视方向/灰阶/全刷效果、触摸、TF 满卡/掉电、内存峰值、睡眠/耗电：未验收。
- 历史开书崩溃已修复，设备离线阅读闭环基线已通过；最终候选的重启恢复已通过，画布与物理显示的验证边界见本轮记录。
  用户已明确取消完整备份。保留历史崩溃 ELF 和日志供追溯。
- 官方完整 KOReader 客户端与本地测试服务联测已通过；外部账户服务和 Read Pico 的 Wi-Fi/HTTPS
  双向接续仍未验收，不能用客户端本地联测代替。
- 精确同步已扩大到普通 CSS、DOCTYPE 和已对照的 HTML 文本；媒体、复杂 CSS/DOM 等仍拒绝同步定位，
  本地阅读按明确降级范围处理。PNG/JPEG 解码及普通 SVG 单图封面已实现，复杂 SVG 与其他不支持格式明确占位。
- 设备上的 Wi-Fi/账户输入、无线导入及详情内目录入口仍未实现；书籍管理与同步界面见顶部最新批次记录。
- PDF、Karakeep、批注/词典、插件、统计、多主题、自定义按键与完整 OTA 仍按既定决定后置。
- 用户现有 IDF 局部修改保持原样；未在全新官方工具链复现；远端 CI 未执行；总许可证及发布义务尚未完成。

## 下一步

按 [最新交接](handoff-next-session.md) 和 [设备验证](device-validation.md) 完成剩余物理目视、触摸、睡眠及 TF 异常验收。
用户准备网络与账户配置后，再开展真实设备 Wi-Fi/HTTPS 接续验收。
扩大同步兼容性必须增加真实引擎对照，不能放宽为百分比或近似位置。

## 本轮记录

### 2026-10-06 · USB 文件传输设计

- 用户确认设置中手动开启 USB 文件传输，默认保持正常阅读和 USB 烧录连接，接受退出时重启。
  具体交互、TF 独占交接、失败处理和验收范围见 [已确认设计](superpowers/specs/2026-10-06-usb-file-transfer-design.md)。
  [实现计划](superpowers/plans/2026-10-06-usb-file-transfer.md) 已拆为存储平台、App、主循环和真机验收四项，等待计划审阅及执行方式选择。
- 已检查设置入口、主循环和固定 BSP；现有 SD 同步接口不能传播卸载失败，接入前需补齐可验证的交接接口。
- 本轮仅新增设计与计划，未修改固件或设备。`python3 tools/check_handoff.py` 289 项通过；`git diff --check` 通过。
  书籍与网页文章的设备目录整理仍未执行，待 USB 模式实际验证后继续。

### 2026-10-06 · Kindle 书籍与字体导入设备

- 经用户明确授权，通过现有 USB 资源导入协议向 Read Pico TF 卡新增 71 个非空 EPUB 和 21 个 KOReader 字体，
  共 86,229,123 字节。92 个文件均完成提交及正式文件长度/CRC32 读回校验；32 个空 EPUB 跳过。
  书籍位于 `Books/Kindle/`，字体位于 `PicoPage/fonts/KO-*`；过长书名在设备文件名中截短，原 EPUB 内容保持不变。
- 实际命令：使用 ESP-IDF Python 运行 `.local/kindle-export-check/import_device.py`，复用 `tools/device_console.py`，
  退出码 0；传输及校验累计约 591 秒。完整原名映射、摘要与日志均保存在已忽略的 `.local/kindle-export-check/`。
  原导出目录、已有设备资源及其阅读记录未修改；未导入凭据或 KOReader 进度，未刷写固件。
- 导入后重启刷新字体列表；`smoke_device.py` 抽测 6 本新书，开书、K3/K1 往返和 K2 全刷均完成驱动确认，
  返回画布一致、保存成功、全刷不改变修订号。此前图片路径失败样本也能开书，但未据此认定其图片正确。
- `font_device.py` 在两份新文章中分别应用 `KO-NotoSerif-Regular.ttf` 和 `KO-NotoSansCJKsc-Regular.otf`，
  仅保存单书设置；两者均通过往返翻页、K2 和重新开书的画布/修订号检查。全局默认未更改。
  设备画布确认英文新字体生效；`capture_body.py` 翻到中文正文并导出 `noto-cjk-body.pgm`，
  中文字形、完整行与页脚目检正常。最终停留在中文测试文章第 3/4 页，修订号 8，画布 CRC32 `2878ab9c`。
  长字体文件名在排版界面显示不全，记为待改进项。所有串口测试进程均已退出。
- 边界：仅抽测开头页面，不是 71 本整书验收；不是 Kindle 原进度接续验证。设备接口未提供 TF 剩余空间查询。
  物理显示、实体输入、复杂书籍完整排版和其余字体仍待验证。导入成功不能代替阅读兼容性通过。

### 2026-10-06 · Kindle 导出数据初步抽测

- 用户授权将本地 Kindle 导出的 KOReader 数据用于测试。原目录只读；个人 EPUB、阅读记录及测试日志不纳入 Git。
  清单和详细结果保存在已忽略的 `.local/kindle-export-check/`，后续兼容性验证优先复用这些真实样本。
- 清点：103 个 EPUB，其中 32 个为 0 字节；其余 71 个可识别为 ZIP（未据此认定内容完整）。
  21 份 `metadata.epub.lua` 均包含保存位置，`cre_dom_version` 为 `20240114`，字体字段为 `Noto Serif`。
  仅用文本匹配提取指定字段，未执行导出的 Lua 文件，也未读取同步凭据。
- 实际执行：`sh tools/check_public_books.sh <现有文楷字体> <本地书籍>` 构建并运行生产解析/绘制探针；
  随后以 `.local/public-books/probe <字体> <样本>` 抽测两份中英文文章和一个空文件。
  两份文章在 28、38、56 像素字号下全部通过分页、绘制及内部源位置往返检查，缺字数均为 0；
  页数分别为 11/17/35 和 24/44/100。空文件明确拒绝，退出码 1。
- 一个书籍样本完成 28 像素字号的 127 页检查后，在后续图片检查中报 `Image 0:0: Invalid internal image path`，
  退出码 1；具体原因待定位，不能将该书记为通过。日志为 `book-probe.log`。
- 验证范围：仅本地 host 探针抽测；未做全部书籍检查、实际 App 交互、导出阅读位置恢复、参考引擎接续或真机验证。
  未修改原数据、固件、设备或同步账户。下一步先定位该真实样本的图片路径失败，再验证已有阅读位置的精确映射。

### 2026-10-06 · 字形缓存淘汰策略（偶发慢页）

- 现象：连续翻页时约每 20 页出现一次 1.5 秒左右的页面准备。设备探针显示慢页与 PSRAM 空闲回升约 0.93 MB
  同时出现（`.local/device/install/slow-page-probe.log`），原因是字形位图缓存达到 1 MiB 后整体清空，当页全部重新栅格化。
- 修改：缓存满时只淘汰最久未绘制的一半，当前页的字形保留；字宽缓存上限由 1,024 提高到 8,192 项（满时仍整体清空）。
- 验证：host CTest 9/9、sanitized CTest 9/9；`page_compare` 的 2,060 页边界与像素摘要仍与基准相同
  （`.local/glyph-cache-check/pages-lru.log`），ASan/UBSan 版本跑《阿Ｑ正傳》三档字号无报错。
  固件构建成功；镜像 `.local/device/install/pico_page-glyph-lru.bin`，1,971,840 字节，
  SHA-256 `6af21f043ce33c8c608945f848f26a7ca20ab5f1f285c9519b958bb6ea48ab79`，仅写 Read Pico `ota_0`，摘要校验成功。
  本条取代此前的已安装镜像。
- 设备结果：《吶喊》连续 50 页，淘汰发生在第 20、33 页（PSRAM 回升约 0.47、0.52 MB），当页准备 0.56、0.29 秒；
  全程最长 1.0 秒（开书后第 3 页，缓存尚未预热），之后多为 0.15–0.6 秒；回翻 50 页回到起始画布 CRC。
  `glyph-lru-probe-nahan.log`。PSRAM 空闲最低约 2.8 MB。
- 测试副作用：《阿Ｑ正傳》的阅读位置被本轮探针翻到书末，回翻 90 页后未能对上测试前的页面，原位置未恢复；
  《吶喊》已回到测试前的页面。两书的修订号因测试增加。
- 未验证：字宽缓存达到 8,192 项时的清空耗时（两本样书未触及）、更大字号下的淘汰频率。

### 2026-10-06 · "快速"刷新档接入八灰阶波形

- 用户目视八灰阶波形后确认有少量残影、可用刷新策略控制，并选定方案：`display.refresh_preset = "fast"`
  改为八灰阶 GL16，自动全刷间隔由 20 次正文更新改为 5 次；"清晰""均衡"不变。
  刷新设置页"快速"的说明改为"翻页使用更快的灰阶更新，每 5 页执行全刷。"，所用字符均在内置提示字体内，布局未改。
- 波形由应用按档位通过 `platform::set_page_waveform()` 选择；上一条记录中的诊断命令 `wave` 和阈值黑白选项已删除。
- 实际执行与结果：host CTest 9/9、sanitized CTest 9/9；`sh tools/build_firmware.sh` 成功（`.local/firmware-fast-preset.log`）。
  镜像 `.local/device/install/pico_page-fast-preset.bin`，1,970,784 字节，
  SHA-256 `e21269ff795567b85b4485640c67bdc2435cf2d2cc7d5ab4e42dc1b0f00e5812`；仅写 Read Pico `ota_0`，摘要校验成功，
  写入前核对端口 USB 序列号与芯片 MAC（`fast-preset-flash.log`）。本条取代此前的已安装镜像。
- 设备验证（诊断输入，`fast-preset-validation.log`）：经工具栏 → 刷新 → 快速 切换后，驱动刷新由 480 降到 376 毫秒；
  连续 13 次 K3 中每 5 次灰阶更新后出现一次全刷（1,363 毫秒）；呈现阶段约 0.62 秒，页面准备多为 0.36–0.90 秒，
  其中一页为 1.47 秒，原因未查。**验证时已把《阿Ｑ正傳》的单书刷新档位设为"快速"并写入 TF 卡。**
- 未验证：5 页间隔下的残影观感（等待用户目视）、其他档位的面板实拍、自动休眠。

### 2026-10-06 · 呈现分段计时、竖屏转换与波形对照

- 诊断 `status` 新增最近一次呈现的 `copy_ms`、`power_ms`、`draw_ms`；新增诊断命令 `wave 0|1|2`，
  选择非全刷帧的波形（0 默认 16 灰阶 GL16、1 8 灰阶 GL16、2 阈值 DU）。该选择不保存，重启恢复 0；
  全刷始终使用 GC16。尚未接入用户设置，等待用户目视比较后决定。
- 分段计时显示竖屏转换占 347 毫秒：原实现按列跨行读取，超出 64 KiB 数据缓存。改为 64 列一组后为 75 毫秒；
  host 上用随机帧核对新旧实现输出相同（临时程序，未入库）。
- 设备实测（诊断输入，`.local/device/install/waveform-probe.log`、`banded-copy-probe.log`）：
  驱动刷新 16 灰阶 480 毫秒、8 灰阶 376 毫秒、阈值 272 毫秒；全刷 1,364 毫秒。
  默认波形下呈现阶段由约 0.99 秒降到约 0.72 秒，8 灰阶约 0.61 秒。
- 已安装镜像：`.local/device/install/pico_page-banded-copy.bin`，1,972,064 字节，
  SHA-256 `31035a80120fde767020fb58addcac7387abb5b01341cdfb7621d1f5e88bf537`；仅写 `ota_0`，摘要校验成功，
  写入前核对端口 USB 序列号与芯片 MAC（`banded-copy-flash.log`）。本条取代此前各条的已安装镜像。
- 本批只做了固件构建和设备计时；host CTest 未重跑（改动限于平台层和诊断组件，host 不编译这两部分）。
- 未验证：各波形的文字边缘、残影和灰阶观感，需用户对着面板判断；自动休眠仍未在设备上验证。

### 2026-10-06 · 字形位图缓存

- `Font::draw` 按"字号 + Unicode 标量"缓存 FreeType 栅格化结果（8 位覆盖率，与原绘制路径逐像素相同）。
  每个字体对象上限 1 MiB，超限时整体清空；重新打开字体时清空。排版、锚点和保存逻辑未改。
- 实际执行与结果：

| 命令 / 操作 | 结果 |
|---|---|
| host 构建与 `ctest --test-dir build/host` | 9/9 通过；`graphics_test` 新增缓存命中与首次栅格化像素一致的用例 |
| sanitized 构建与 CTest | 9/9 通过，无 ASan/UBSan 错误 |
| `.local/font-psram-check/page_compare.cpp` 链接当前库，两本鲁迅 EPUB × 三档字号 | 2,060 页的分页边界与像素摘要和 `.local/font-psram-check/pages-after.log` 完全相同；`.local/glyph-cache-check/pages.log` |
| `sh tools/build_firmware.sh` | 成功；1,965,408 字节，SHA-256 `48e79332c22f1790fd65004c0add89497dd733c1d33d4683ddc4a06bc8e456c6`；`.local/firmware-glyph-cache.log` |
| 仅写入 Read Pico `ota_0`（`0x10000`） | `Hash of data verified`；写入前核对端口的 USB 序列号与芯片 MAC；`.local/device/install/glyph-cache-flash.log` |
| 设备翻页计时（诊断输入，《阿Ｑ正傳》） | 见下；`.local/device/install/latency-glyph-cache.log` |

- 设备计时（页面准备 + 面板呈现）：改前每页约 1.27 + 0.99 秒；改后连续向后 8 页的准备时间
  从 1.02 秒降到 0.35–0.59 秒，回翻到已缓存的页为 0.09–0.10 秒；面板呈现仍约 0.99 秒（全刷约 1.8 秒）。
  往返各页 CRC 一致，K2 不改变修订号。这 17 页后 PSRAM 空闲从 4.33 MB 降到 3.73 MB，内部空闲约 148 KB 不变。
- 当前已安装镜像为本条的 `48e79332…`，取代此前各条记录的镜像。
- 未验证：缓存满后整体清空时的单页耗时、长时间阅读的内存峰值、面板实拍。自动休眠仍未在设备上验证。
- 下一步：面板呈现的约 1 秒是当前主要耗时；可选做空闲时预渲染下一页，或由用户对着屏幕比较更快的波形。

### 2026-10-06 · 章节解析分配、空闲降频与自动休眠

- 章节解析不再为每个字符构造来源锚点字符串，改为每个文本节点一次；隐藏内容直接跳过。
  正文链接下划线只处理与当前页相交的链接。输出不变，见下方对照。
- 新增全局设置 `power.auto_sleep_minutes`（0–240，默认 10，0 关闭），只能在 TF 卡上编辑，
  启动或开书时读取。主循环在无物理输入、无诊断命令、无网络任务和导入时计时，到时走既有休眠路径。
  **TF 卡上现有 `settings.toml` 没有此项时按 10 分钟生效；休眠后 USB 串口会断开，需按电源键唤醒。
  无人值守的串口验证前，先把此项设为 0。**
- 启用 `CONFIG_PM_ENABLE`：活动时持有最高频率锁（240 MHz），无活动 3 秒后释放，空闲频率 80 MHz；
  `present()` 总是先恢复最高频率。未启用自动浅睡（tickless）。
- 启用 `CONFIG_FATFS_USE_FASTSEEK`，只对只读打开的文件生效；簇链表建立失败时 IDF 回退到普通 seek。
- 实际执行与结果（工作目录 `/Users/wulu/workspace/pico-page`）：

| 命令 | 结果 |
|---|---|
| `cmake --build build/host -j 8`；`ctest --test-dir build/host --output-on-failure` | 9/9 通过；`store_test` 新增自动休眠设置的默认值、0、越界和单书拒绝用例 |
| `cmake --build build/sanitized -j 8`；`ctest --test-dir build/sanitized --output-on-failure` | 9/9 通过，无 ASan/UBSan 错误 |
| `sh tools/check_public_books.sh <文楷> <两本鲁迅 EPUB>` | 各章字符数、页数、节点数、来源段数与 `.local/font-psram-check/pages-after.log` 的非像素行逐行相同；`.local/parser-alloc-check/` |
| `python3 tools/check_koreader_mapping.py` | 退出码 0，各组 2018/2018 精确源位置通过；`.local/parser-alloc-check/koreader-mapping.log` |
| `sh tools/build_firmware.sh` | 成功；`firmware/build/pico_page.bin` 1,964,144 字节，SHA-256 `8e050edba93be7d17f3242604f7d4f3a731573664fd920a3611fa953369c165d`；`.local/firmware-idle-power.log` |

- 经用户同意，本轮镜像已仅写入 Read Pico 的 `ota_0`（`0x10000`），`Hash of data verified`；
  顶部"当前产物"中的镜像大小和摘要已被本条取代。目标确认方式与设备结果见 [设备验证](device-validation.md)。
- 设备结果（诊断输入）：启动、书库全刷、重开《阿Ｑ正傳》恢复到修订 30 的原页；K3/K1 翻页每次 2.4–2.8 秒
  （含一次周期全刷的为 3.3–4.2 秒），K1 返回各页 CRC 与去程相同；空闲 5–8 秒（已释放最高频率锁）后的
  翻页和 K2 全刷均成功，K2 不改变修订号。上一轮同路径记录为 3.1–3.4 秒，未单独区分各项改动的贡献。
- 卡上的 `auto_sleep_minutes` 未能预先设为 0：诊断接口没有写配置文件的命令，本轮按默认 10 分钟运行。
- 用户决定本轮不测休眠：自动休眠的触发与唤醒均未在设备上验证，仅有 host 的设置解析测试。
- 未验证：实体触摸在 80 MHz 空闲时的响应、实际电流。这些需要人工操作或仪器。
- 未做：TF 卡章节分页缓存。字宽缓存后重排只在内存中进行，先在真机测量往回翻的耗时再决定。
- 下一步：烧录本轮镜像后，用诊断 `status` 记录翻页各阶段耗时和休眠前后电流，再决定是否精简进度保存的回读。

### 2026-10-06 · 开书修复与设备离线阅读闭环

- 原开书崩溃由 ESP32-S3 ROM miniz 替换项目同名函数造成，解压结构体 ABI 不兼容。
  使用组件符号前缀修复 EPUB 和 PNG 调用，保留 vendor 原件，未增大主栈。
- 字体分配器经过正常打开、中文绘制、分配失败、重分配失败/零尺寸清理验证；普通分配改为
  PSRAM 优先，设备阅读内部空闲约 149 KB、最大连续块 100 KiB，不再出现原 TF DMA 分配失败。
- 默认文楷路径与准备脚本一致；补齐全局排版保存的覆盖/继承反馈，保持 TF 配置权威。
  有界字宽缓存不改变已验证的 2,060 页边界与像素摘要；提示子集为 410 字符。
- 实际执行：`cmake --build build/host -j 8`、host CTest 9/9；相同 sanitized 构建和 CTest 9/9。
  日志 `.local/host-metrics-cache.log`、`.local/sanitized-metrics-cache.log`。
- `sh tools/build_firmware.sh` 完整构建成功；仅向实测 OTA0 写入最终应用，Flash 摘要验证成功。
  构建与烧录证据见 [设备验证](device-validation.md)。没有提交、推送或发布。
- 真机经诊断输入完成两书开读、前后翻页、K2、排版取消/应用、默认继承/覆盖、目录返回、
  切书恢复与硬复位后同页恢复。最后保存修订 24 与正文 CRC `d1b7e837` 在重启前后相同。
  `.local/device/install/final-validation.log`；最终画布 `artifacts/runtime/read-pico-offline.png`
  已检查，中文正文与页脚未见裁字。画布导出不等于物理成像验收。
- 用户明确先完成离线阅读；网络/服务配置、设备 Wi-Fi/HTTPS 双向接续暂缓。
- 收尾 `python3 tools/check_handoff.py` 通过 288 项，原设计 49 文件及 3 个历史快照未变；
  `git diff --check` 通过，安装镜像与当前构建 SHA-256 一致，串口客户端已退出。
  物理目视、触摸、睡眠、TF 满卡/掉电与长期功耗仍未验收；不将这些事项标为通过。
- 下一步：按真实硬件使用反馈修正问题；具备网络配置后再做设备与 KOReader 双向联测。


### 2026-10-06 · 图片、导入与设备应用安装

- 原生 EPUB 接入有界 PNG/JPEG 解码、普通 SVG 单图封面与独立图片分页；保留元素来源锚点，
  损坏或不支持的媒体显示明确占位。内置提示字体更新为 406 字符，覆盖当前 App 文案。
- 完整 `sh tools/build_firmware.sh` 成功，日志 `.local/firmware-usb-protocol.log`；
  镜像大小与摘要见顶部。host 与 ASan/UBSan CTest 各 9/9 通过，交接检查 284 项通过。
- 官方完整 KOReader 客户端六组双向接续和重启恢复通过；仍只证明本地测试服务联测范围。
- 用户取消完整备份后，按已确认布局仅写入 `ota_0` 应用；写入摘要校验通过，未改 bootloader、
  分区表、NVS、otadata 或工厂校准。此前首版实际启动已确认 PMU、VCOM 读取、SD、PSRAM 与书库全刷。
- 新镜像诊断与资源导入正在验证；没有将后续操作写成通过。物理目视、触摸、睡眠和实际设备同步仍待验收。
- 当前 main 工作目录保留全部改动，未提交、推送或发布；原设计与历史快照未修改。

### 2026-10-06 · 真实 EPUB、设备预检与诊断接入

- 取得未修改的《阿Ｑ正傳》和《吶喊》，来源、摘要与许可保存在 [公开样本记录](public-books.md)。
  两书实际逐页绘制通过。App 使用《阿Ｑ正傳》验证保存后重开、K2、工具和模拟睡眠恢复，
  修订号保持 6；全刷前后与恢复后的 BMP 摘要相同，证据 `.local/lu-xun-resume.log`。
- 原文小标点及引号纳入换行禁则，原文已有全角缩进时不再叠加默认缩进；没有更改视觉布局。
- EPUB 有界保留样式和首文本子节点元数据；同步支持普通 CSS/DOCTYPE 与块首空白索引变化。
  扩大的真实引擎检查同时核对逐字符值和实际 `getBookmark()` 反向位置，详见同步记录。
- 接入默认关闭的 USB 诊断组件。主循环统一物理/诊断输入序号，只有真实面板完成后记录帧结果。
  主机客户端支持安全命令和带帧号/CRC 的当前画布导出；无 Flash、NVS、校准或任意文件写入口。
- 最新 host 与 ASan/UBSan 各 9/9 通过。变更 C++ 已做 Xtensa 目标文件编译；这不等于完整链接。
  完整 IDF 重建因沙箱禁止依赖管理器 `sysctl` 查询而停止；当前 bin 仍为前次完整构建。
- 硬件只读确认 ESP32-S3、8 MiB PSRAM、16 MiB Flash 和双应用槽；未改任何 Flash 字节。
  全片读取首次传输失败，低速分区读取成功；完整备份重试因自动审批服务用量已达上限而未执行。
  审批失败不是安全性拒绝；未绕过审批，未重启退出下载模式，未宣称设备正在运行 Pico Page。
- 工作继续保留在当前 main，未提交、未 push、未发布。完整可用固件目标仍未达成。

### 2026-10-05 · 代码注释规范
- 改动：在 `AGENTS.md` 中加入完整的 Code Comment Policy，明确注释内容、措辞、API 文档及修改时的清理要求；面向用户的说明参考 Tech-Doc-Style-Chinese，补充中文表达、操作步骤、事实保真和排版原则，并明确 ASD-STE100 方法的中文适用边界。
- 实际执行命令与工作目录：在 `/Users/wulu/workspace/pico-page` 执行 `python3 tools/check_handoff.py`，248 项交接包检查通过。
- 验证范围：仅文档与交接包检查；未新增测试，未执行固件构建、host 或真机验证。
- 当轮限制：修改规范时目录尚未初始化 Git，无法检查 Git 差异；现已完成初始化，见下方记录。
- 新的兼容性、迁移或许可证风险：无，仅更新开发规范。
- 下一项最小工作：继续按 `CODEX_START.md` 推进 G0 + G1a。

### 2026-10-05 · 初始化本地仓库
- 改动：执行 `git init -b main`，建立本地 Git 仓库；按现有 `.gitignore` 纳入当前交接资料、设计原件、示例与检查工具。
- 实际执行命令与工作目录：在 `/Users/wulu/workspace/pico-page` 执行 `python3 tools/check_handoff.py`，248 项交接包检查通过；常见凭据模式与个人 EPUB、二进制备份、密钥、日志文件名检查未发现匹配项。
- 验证范围：仅资料完整性与提交内容检查；固件构建、host 和真机验证仍未执行。
- 远端状态：未配置远端，未推送。
- 下一项最小工作：继续按 `CODEX_START.md` 推进 G0 + G1a。

### 2026-10-05 · G0 / G1a 实现与 G1b 探针

- 改动：完成上述 core/store 和诊断工程，新增构建脚本、host CI、合成 EPUB 探针与逐项依赖审查。
  未修改 49 个原设计文件或三份历史快照；没有新增占位 UI、自定义按键、网络成功状态或专有阅读包。
- 工作目录：`/Users/wulu/workspace/pico-page`。所有改动保留本地，未提交、未发布、未 push、未刷写。
- 实际命令和结果：

| 命令 | 实际结果与证据 |
|---|---|
| `cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Debug` | 配置通过；AppleClang 21 / C++17 / C17 / Python 3.14.7 |
| `cmake --build build/host -j 4` | 退出码 0 |
| `ctest --test-dir build/host --output-on-failure` | 5/5 通过；`build/host/Testing/Temporary/LastTest.log` |
| sanitizer 配置命令见 `docs/environment.md`，随后构建并运行 CTest | 5/5 通过；`build/sanitized/Testing/Temporary/LastTest.log`；无 ASan/UBSan 运行时错误 |
| `sh tools/build_firmware.sh` | 退出码 0；`firmware/build/pico_page.bin`，162064 字节；`.local/firmware-build-final.log` |
| `python3 -m unittest discover -s tests -p test_epub_probe.py -v` | 四组 Python 检查通过，亦已纳入 CTest |
| `python3 tests/make_epub_fixture.py /tmp/pico-page-probe.epub`；探针 `--spine 0` / `--spine 1` | 原创两章 EPUB 解析成功；摘要和标量证据见 `docs/epub-kosync-probe.md` |
| `python3 tools/check_handoff.py` | 原设计摘要、示例及本地文档链接检查通过，仅交接范围 |
| `git diff --check`；`sh -n tools/build_firmware.sh tools/setup_bsp.sh` | 均退出码 0 |

独立代码审查找到并修复了目录同步失败后重试错误确认保存的问题。
`store_sync_test` 先复现失败，再验证持续失败不返回确认、故障解除后才成功；复审确认原问题已解决。
同时修正 FatFs 不支持覆盖重命名的发布协议、无效 UTF-8 读取，以及 tomlc17 的 Xtensa 打印格式。
这些都是实际失败或源代码约束；没有用成功占位掩盖问题。

| 验收项 | 本轮覆盖与限制 |
|---|---|
| IN01–IN03 | `pass_host_only`：集中映射和全刷意图、忙碌去重、面板安全动作；物理全刷及 UI 仍待 D/V |
| IN04 | `pass_host_only`：序列尾部与获准唤醒输入不穿透；实际唤醒来源待 D |
| EV01–EV02 | `pass_host_only`：已实现的布局/呈现/保存代次，失败/旧请求不推进进度；网络回包尚无实现 |
| ST01 | `pass_host_only`：缓存清理保留原书、记录、配置、封面与 prepared；真实离线阅读待内容/设备接入 |
| ST02 / ST07 | 部分 host 覆盖：临时文件不恢复、坏/缺主文件恢复、写入受阻/短写/同步失败；未穷举 syscall，未做 TF 掉电/满卡 |
| ST03 | 部分 host 覆盖：已核实内容身份不符时拒绝复用；原书摘要核实仍由调用方负责 |
| CFG01 | `pass_host_only`：语法、UTF-8、字段、版本错误有诊断且保留原文 |
| CFG03 | 部分 host 覆盖：本书覆盖与全局继承；删除覆盖/界面应用未实现 |
| EP03–EP05 | 部分 host 探针：源标量、章节隔离、资源限额和显式拒绝；没有分页、图像或设备内容后端 |
| SYNC01–SYNC03 | `not_run`：真实客户端位置往返未执行；上游 MD5 函数一致不算同步完成 |
| DP/PW/其他 D/K/V | 未执行，不由编译或 recording display 代替 |

兼容性和许可：BSP/SDK 存在多种组件许可，最终二进制分发义务未完成；项目总许可证待选。
G0 的硬件门槛、G1b 原生定位、G2 阅读界面、G3 同步和 G4 发布均未完成。
下一步按本页顶部顺序推进，不重新讨论正文 K2 和既有设计。

## 每轮追加记录模板

### <日期> · <任务/Git commit（若有）>
- 改动：
- 实际执行命令与工作目录：
- 测试结果与证据路径：
- 仅编译通过 / host 验证 / 真机验证分别是什么：
- 未验证/阻塞（不得写成通过）：
- 新的兼容性、迁移或许可证风险：
- 下一项最小工作：

真实结果应来自当前运行，不能沿用交接包检查的“通过”填充固件完成率。

### 2026-10-07 · 外部设计评审图片导出
- 产物：`artifacts/ui-review-20261007/`，204 张 PNG、两张页面总览、HTML 分类索引、来源清单和说明；压缩包为 `artifacts/pico-page-ui-review-20261007.zip`。
- 覆盖当前全部 23 类 `View`，另有手机配置与 Wi-Fi 传书网页 6 个状态。设备图为 684 × 1216 的实际 C++ 画布；正文重复页仅取代表样本，不代表穷举所有数据组合或原设计历史状态。
- 来源：`d6a3b329` 加工作区未提交改动。导出期间发现 Wi-Fi 传书页面新增，已补入；未修改应用代码、原始设计或用户数据。
- 实际执行：`cmake --build build/host --target app_test pico_page -j 8` 成功；`.local/ui-review-20261007/capture.py` 复用当前 `app_test.cpp` 导出画布，原有断言通过；独立临时卡通过 `pico_page --headless` 补充中文演示和三种休眠画面。生产 HTML 经隔离无头 Chrome 截图。
- 检查：204 张图片均可解码，全部设备 PNG 尺寸核对通过，23 类页面覆盖核对通过，ZIP 校验通过；目视检查两张总览及正文、手机表单、传书引导原图。
- 边界：合成 EPUB、模拟网络回包及设备状态；桌面容量与电量显示不能作为硬件值。不是面板实拍、联网或真机验收；未刷机、未提交、未推送。
- 下一步：由外部设计师基于当前实现评估界面，收到评审结论后再决定修改范围。
