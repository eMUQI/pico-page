# 设备诊断

诊断构建通过 USB Serial/JTAG 提供本地串口协议，供 `tools/device_console.py` 操作设备、`tools/device_workbench.py` 采集证据。协议没有账户认证，只用于受控设备；正式构建不包含这些命令。命令参数以各工具的 `--help` 为准。

## 构建模式

先按[环境说明](environment.md)激活 ESP-IDF 6.1，在仓库根目录运行 `sh tools/build_firmware.sh <模式>`。每种模式使用独立的 `build/firmware-<模式>/`，由板级 defaults 加 `firmware/configs/<模式>.defaults` 生成 sdkconfig；无参数时为标准构建。

| 模式 | 诊断串口 | 聚合指标 | 事件追踪 | 用途 |
|---|---|---|---|---|
| `release` | 关 | 关 | 关 | 发布 |
| `performance` | 关 | 开 | 关 | 不受串口查询干扰的性能对照 |
| `diagnostic` | 开 | 开 | 开（64 个事件） | 串口操作与工作台 |
| `stages` | 开 | 关 | 关 | 启用旧阶段计时日志（`LEGACY_PERF_LOG`），输出章节与页面阶段耗时 |

指标调用使用 `PP_DIAGNOSTIC`，关闭时连参数求值和时钟读取一起删除。记录器只在任务上下文调用，跨核临界区内只更新固定数据，不做 I/O、分配或等待。时间单位为单调微秒，嵌套阶段的墙钟不能相加。阶段计时会增加开销，须与同源普通版对照；串口查询期间的耗时不能代替 Performance 构建的结果。

## 串口协议

命令为 UTF-8 单行，格式 `PP <id> <command>`，最长 4224 字节；收到同一 ID 的 `OK` 或 `ERROR <code>` 后再发下一条。超长或含控制字符的行整行丢弃，并取消活动导入。

| 命令 | 行为 |
|---|---|
| `status` | 视图、待呈现状态、已保存修订号、帧结果、画布 CRC32、内存与栈余量、页码与统计状态、同步面板结果；不输出正文、锚点原文或凭据 |
| `ls <起始项> <目录>` | 列出卡内安全相对目录，每页最多 32 项，名称以十六进制返回 |
| `open <路径>` | 经正常应用路由开书 |
| `key K1\|K2\|K3`、`tap x y`、`hold x y`、`press x y`／`release x y`、`swipe left\|right` | 经应用路由注入输入；不经过触摸采样和时长判定，不能证明实体输入正常 |
| `waveform default\|ripple` | 选择本次启动的翻页效果，不保存配置 |
| `screen <帧号> <偏移> <长度>` | 导出指定已确认帧的画布字节 |
| `file <路径>`、`file_head <路径>` | 返回白名单资源的长度与 CRC32，或首部最多 256 字节 |
| `begin`、`data`、`commit`、`abort` | 资源导入事务 |
| `metrics`、`trace`、`trace_start`、`trace_stop` | 读取聚合指标和事件，或开始、冻结一个采集窗口 |

`status`、`ls`、`screen`、`file`、`file_head` 和指标类命令是观察命令，不重置空闲计时，不影响停留保存、页数统计和休眠。网络工作、休眠请求或物理触摸未结束时，开书和导入被拒绝；有待呈现帧时输入返回 `ERROR busy`。

每次真实显示提交另发 `PP 0 FRAME number=… success=… purpose=… clean=…`。输入命令的 `accepted` 只表示已交给应用；`FRAME success=1` 是驱动结果，不证明物理成像。

**屏幕导出**：画布 684 × 1216，每行 342 字节，偶数 x 在低半字节，灰度 0 黑、15 白。先读 `status` 确认 `pending=0`、`frame_ok=1`，再按帧号分段导出并核对长度与 CRC32；帧号变化时拒绝导出。

**资源导入**：只接受 `Books/` 下的 EPUB、TXT，`Fonts/` 下的 TTF、OTF 和许可证文本，以及 `Dictionaries/` 下的 StarDict 文件，1 字节至 64 MiB，不覆盖已有文件。写入使用排他临时文件，核对长度与 CRC32、同步并读回后才发布。30 秒无活动或卡不可用时终止事务。没有覆盖、删除、格式化、NVS、校准、重启或休眠命令。

**限制**：浅睡期间 USB 不响应，唤醒后需重新打开串口。USB 文件传输与诊断串口分时使用同一 PHY，传输开启时诊断连接停止。

## 主机工具

`device_console.py` 需要 pyserial，使用 ESP-IDF Python 环境运行。它按 IDF Monitor 的无复位顺序打开串口，连接后先重试只读 `status`，再执行动作。

```sh
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE status
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE open Books/sample.epub
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE key K2
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE capture .local/device/frame.pgm
python3 tools/device_console.py --port /dev/cu.TARGET_DEVICE upload sample.epub Books/sample.epub
```

端口按当前枚举结果选择。`capture` 逐块核对帧号与 CRC，不覆盖已有输出文件。

## 指标与事件

| kind | 覆盖范围 | success 的含义 |
|---|---|---|
| 0 Frame | `platform::present` 的调用 | 驱动返回成功，不证明物理屏幕 |
| 1 Input | 实体输入经应用分发 | 路由函数返回，不代表业务接受 |
| 2 Network | 同步请求或更新任务 | 同步为 HTTP 2xx；下载为镜像验证成功 |
| 3 Content | 内容任务从提交到返回 | 无已报告错误；保存任务另查提交状态 |
| 4 ImagePatch | 渐进图片复制到画布并请求局部刷新 | 刷新请求被接收；须再关联成功的 Frame |
| 5 NetworkStage | 联网、校时、HTTPS、解析、下载等阶段 | 该阶段包装函数返回成功 |

每项聚合 started、active、成功／失败／取消／超时计数及耗时总和与最大值；资源低水位取 ESP-IDF 的内部 RAM 与 PSRAM 历史最低余量。事件携带会话、排版、帧和请求代次，用于关联内容、图片与驱动帧。

## 工作台

`device_workbench.py` 独占串口，持续保留原始字节，按状态和事件等待，不使用固定延时，也不绕过忙态保护。点击位置取自源码中的控件坐标，界面布局改变后须同步更新工具顶部常量。

| 模式 | 内容 |
|---|---|
| `manual` | 持续采集，操作者输入 `mark 说明` 标记问题，`end` 结束 |
| `refresh` | 正文页注入 K2，断言全刷成功且视图、页码、保存修订和画布 CRC 不变 |
| `image-network` | 人工发起同步时，核对图片内容任务、ImagePatch 和驱动帧的证据链 |
| `image-sync` | 自动构造图片解码与同步请求交叠，验证取消和旧结果不被采用 |
| `reading-flow` | 多轮翻页、改字号再改回、返回书库重开，核对锚点、修订号和内部 RAM 稳定 |

每轮需要 `--metadata` 私有 JSON，写明硬件、TF 卡、测试书摘要、冷／热缓存、网络条件和 `device_build_verified`；还需要 `--firmware` 指向构建目录，工具核对 `provenance.json`，源码与构建不符时结果降为 inconclusive。

结果分为 pass、fail、skip、inconclusive 和 infra_error。漏事件、溢出、未完成操作、设备重启或缺少元数据时不显示通过。输出目录必须不存在，包含 `run.json`、原始串口、事件、标记和离线报告。`--compare` 只比较两份均为 pass、构建来源已核验且工作负载标识相同的结果。

原始日志可能含路径和服务地址。结果放在忽略目录 `.local/`，不提交真实日志、书籍内容、账户或完整 sdkconfig。
