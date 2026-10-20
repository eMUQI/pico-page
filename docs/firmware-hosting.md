# 腾讯云固件下载

## 当前范围

正式下载已于 2026-10-19 由腾讯云轻量服务器切换至广州 COS：`https://pico-page-firmware-test-1258873179.cos.ap-guangzhou.myqcloud.com/`。桶采用单 AZ、标准存储和按量计费，只存放已发布、带版本号的 BIN；公有读私有写，对象继承桶 ACL，不设置匿名允许策略。未接入 CDN、全球加速或自定义域名。旧 `oc.wulu.zone` 镜像保留为人工恢复入口，不再由新工作流自动更新；正式上线结果以[开发状态](development-status.md)为准。

设备仍从 `https://picopage.wulu.zone/firmware/manifest.json` 读取清单；官网部署副本只将 `url` 改为 COS 的应用镜像地址，保留版本、大小、SHA-256、数据格式和更新日志。固定清单入口不变，GitHub Release 原附件保留。安装页从清单同目录下载全量镜像，并在擦除前校验从发布附件计算的 SHA-256。网站页面、清单及网站内固件副本继续由 Cloudflare Pages 提供；设备与安装页选用 COS 地址，不自动切换回网站副本。

## 广州 COS 下载验证

2026-10-18 创建测试桶并上传正式 `0.3.0` 应用和全量 BIN，完成私有签名下载、Range 和摘要核对。2026-10-19 经用户本人验证后，公有读私有写保存成功；匿名完整下载两个 BIN 分别为 3,524,064 和 3,589,600 字节，SHA-256 与发布附件一致，本机耗时分别为 0.588 和 0.505 秒。无 Referer 的 64 KiB Range 返回 206；错误 Referer、匿名列目录及其他来源的跨域预检返回 403。这些仅为本机短时样本，不证明真机 OTA 或其他运营商的表现。

CORS 仅允许 `https://picopage.wulu.zone` 的 GET/HEAD，请求头允许 `Range`，暴露 `ETag`、`Content-Length`、`Content-Range` 和 `Accept-Ranges`，Max-Age 为 600 秒并返回 Vary。Referer 白名单为 `picopage.wulu.zone`，允许空 Referer 以兼容设备。CORS 不限制非浏览器客户端，Referer 可以伪造，均不替代自动封禁。

COS 的预算告警、资源包和异常流量检测不构成实时费用硬上限；资源包超量仍按量计费，异常封禁前的流量仍计费。计费边界见[预算管理](https://cloud.tencent.com/document/product/555/65784)及[COS 计费计量说明](https://cloud.tencent.com/document/product/436/30747)。

### 费用保护与自动封禁

当前状态（2026-10-20）：因监控异常触发封禁并造成官网下载失败，按用户要求恢复公有读私有写，并停用 `PicoPage-COS-Guard-Minutely` 定时器，刷新控制台后已读回关闭。费用、流量和函数健康告警保留；暂停期间没有该函数提供的自动流量封禁。下文记录原部署机制，不能视为当前仍在定时执行；重新启用需在完成排查、明确监控异常处理策略后另行确认。证据和未确认项见[开发状态](development-status.md)。

2026-10-18 已在控制台创建并读回全账户预算「Pico Page 账户日费用」和「Pico Page 账户月费用」，分别为日 ¥1、月 ¥10，实际费用达到 100% 时告警，持续生效，沿用现有接收人并全天通知。COS 尚未出现在产品筛选中，因此采用用户另行确认的全账户范围；现有其他服务费用也会触发，不能将预算进度视为本次 COS 消费。费用数据约每 2 小时更新，不会自动限制或停用服务。接收人的手机号已验证，邮箱未设置；没有发送测试通知，不宣称实际送达。

流量策略 `PicoPage-COS-traffic`（`policy-n2te4ck5`）已启用，只关联本固件桶。配置为外网下行流量 5 分钟统计超过 100 MiB、持续 1 个数据点、每 5 分钟重复通知；编辑器单位选择 MiB，详情页显示为通用的 MB。沿用系统预设通知模板，不修改其他策略共用的模板；邮件和短信已选，但邮箱未配置。告警本身不执行封禁。

用户已选择「增加自动封禁后切换」。`tools/cos_guard.py` 是已部署的 SCF 定时处理器，每分钟读取 `QCE/COS` 的 `InternetTraffic`（字节，300 秒统计），查询截止当前前 2 分钟的 30 分钟窗口，允许 API 将起点按统计周期向前取整；任一样本超过 100 MiB，或数据缺失、无效、超过 15 分钟未更新及查询失败时，将指定桶 ACL 改为 `private` 并保留错误供监控。它不会自动恢复公开访问；恢复前须确认异常原因并由管理员手动操作。桶内对象必须继承桶权限，不得单独公开对象或添加匿名读取策略，否则桶 ACL 封禁可能失效。该方案存在采集、调度、执行延迟及平台故障风险，不能承诺账单封顶，也不能撤销已签发且仍有效的授权链接。

运行角色的策略见 `tools/cos-guard-policy.json`：仅允许 `monitor:GetMonitorData` 和本桶的 `cos:PutBucketACL`，不授予上传、删除对象或其他桶的写权限。函数从每次调用的 `context.environment` JSON（兼容顶层字段）取得临时角色凭据，不保存长期密钥；源码不接受事件传入的桶名、阈值或恢复指令。首次使用 SCF 的服务配置角色与函数运行角色不同，前者使用腾讯云预设策略，包含更广的服务配置权限。2026-10-19 经用户确认，已创建并启用独立运行角色 `PicoPageCosGuardRole`（`4611686018449702669`），仅信任 `scf.qcloud.com` 并关联 `PicoPageCosGuardPolicy`（`290353907`）。广州函数 `PicoPage-COS-Guard`（`lam-fmuly2ju`）已创建，已修正 Python 3.10 的 context.environment 凭据解析并完成实际监控查询及异常时的私有 ACL 写入。每分钟定时器已读回启用，错误和受限告警 `PicoPage-COS-Guard-health`（`policy-esbjic5w`）已创建；公开到封禁的完整转换已实测通过：临时将阈值设为 -1，真实查询读到 7,180,376 字节，函数返回 `private`，原先 200 的匿名下载变为 403，控制台读回私有。随后恢复正式 100 MiB 阈值并部署，下载代码逐字节核对与本地源码一致，再恢复公开下载，Range 返回 206。

函数包采用 Python 3.10、Linux x86-64 依赖，版本固定于 `tools/cos-requirements.txt`。SDK 和传递依赖不进入固件或网站；应保留各包许可证、元数据和来源记录，禁止将本机 macOS 扩展装入 SCF。`crcmod` 使用其上游支持的纯 Python 回退。本地隔离逻辑回归通过；云端已完成依赖导入、凭据解析、实际监控查询与私有 ACL 写入；公开下载到封禁及正式配置恢复均已验证。首次成功调用 11,872 ms / 85.8125 MB，复用调用 106 ms / 75.4140625 MB，仅为少量观测，不构成长时间费用或可用性保证。上述测试没有执行设备 OTA 或刷写。

发布接入由 `tools/publish_cos.py` 实现：复用发布校验器，只上传带版本号的 BIN，继承桶 ACL，使用服务端禁止覆盖请求头，完整认证读回一致后才允许后续网站部署；不会将桶重新公开。官网清单仍由 Pages 提供，COS 不需要可变的 `manifest.json`。上传身份采用独立 `PicoPageCosPublisher` 角色，经 GitHub OIDC 换取 15 分钟临时凭据；权限 `tools/cos-publisher-policy.json` 仅允许本桶项目 BIN 的读取和禁止覆盖上传，不含删除或 ACL 修改。仓库当前已启用不可变 OIDC 主体，实际前缀为 `repo:eMUQI@32332228/pico-page@1407034423`；信任条件必须限定该前缀的 main 和发布标签，不能使用旧的仅名称主体或允许 PR。`PicoPageGitHub` OIDC 提供商已创建并开启公钥自动轮转，最小上传策略 `PicoPageCosPublisherPolicy`（`290357331`）已保存；独立角色 `PicoPageCosPublisher`（`4611686018449703622`）已创建并只关联该策略，禁止控制台访问。最终信任策略已读回 main 与 v* 标签数组，见 `tools/cos-publisher-trust.json`。脚本请求的会话为 900 秒；控制台角色最大会话期限仍为默认 2 小时，不应将脚本请求时长误称为角色强制上限。真实 Actions 运行 `39186467623` 已通过身份交换、既有对象禁止覆盖重试、完整认证和匿名读回、Pages 发布及正式清单核对；详见开发状态。

## 自动部署

`Website build` 在 main 网站输入变化、main 手动运行或发布 Release 时部署。工作流取回最高非草稿版本的原附件，校验后仅改写部署清单的 COS 地址，构建网站；通过 GitHub OIDC 换取独立发布角色的临时凭据，使用服务端 `x-cos-forbid-overwrite: true` 上传同版本 BIN。发布前先认证读回已有对象并逐字节比较，只有内容完全相同时才复用；仅服务端明确返回 `NoSuchKey` 时上传新对象。上传使用 SDK 文件流分块发送，保持 30 秒单次套接字操作超时，避免整个镜像共用一次写入时限；禁止覆盖请求头、MD5 及上传后的完整认证读回均保留。最后通过匿名 HTTPS 完整读回并比较。仅当 COS 返回 HTTP 403，且候选清单、应用及随包全量镜像均与官网现有副本逐字节一致时，允许继续更新静态页面并在 Actions 明确告警；不改变固件下载地址，不恢复桶的公共权限，也不将下载标记为可用。候选固件或清单变化、官网副本读取失败、内容不一致及其他下载错误均停止部署。认证上传与读回失败仍停止部署。

上述核对通过后才部署 Cloudflare Pages，并独立读回正式域名的部署标识及清单。PR 不执行发布步骤；角色信任不允许 PR、其他分支或其他仓库。新工作流不使用腾讯云长期密钥或 SSH Secrets。旧标签使用旧工作流时，应在 main 手动运行最新流程。回退到剩余最高版本时须确认对应不可变 BIN 可用；不会删除已发布对象，也不创建或刷写新固件。

## 原轻量服务器恢复资料

以下为保留的 `oc.wulu.zone` 配置，非 COS 的运行规则。原服务器及 SSH Secrets 没有删除；仅在明确选择回退并完整核验后使用。

### 文件与访问边界

| 对象 | 服务器路径或规则 |
|---|---|
| 站点配置 | `/etc/caddy/Caddyfile` |
| 下载根目录 | `/srv/pico-page/firmware/` |
| 服务隔离配置 | `/etc/systemd/system/caddy.service.d/10-pico-page-security.conf` |
| SSH 配置 | `/etc/ssh/sshd_config.d/00-pico-page-security.conf` |
| SSH 防爆破配置 | `/etc/fail2ban/jail.d/pico-page-sshd.local` |
| 自动更新源配置 | `/etc/apt/apt.conf.d/52-pico-page-updates` |
| 原配置备份 | `/root/pico-page-setup-20261017/`，仅 root 可访问 |
| 访问日志 | `/var/log/caddy/oc-access.log`，单文件按 10 MiB 滚动，最多保留 5 个归档，归档保留期为 7 天 |

下载根目录属主为 `pico-deploy:caddy`，目录权限为 `0750`，发布文件权限为 `0640`；旧版本文件保留原属主。部署账号仅能写固件目录，Caddy 只读。Caddy 以专用用户运行，服务内的 `/srv/pico-page` 额外挂载为只读；启用 `NoNewPrivileges` 和 `ProtectHome`，仅保留 `CAP_NET_BIND_SERVICE`。证书状态由 Caddy 管理，不放入下载目录。

站点只允许 GET 和 HEAD，只提供 `/firmware/manifest.json` 及符合项目命名规则的应用和全量 BIN。其他路径返回 404，其他请求方法返回 405。不开启目录列表、上传、动态脚本或反向代理；不要将符号链接、仓库、凭据、日志或备份放入下载根目录。

清单使用 `Cache-Control: no-store`。文件响应允许官网 `https://picopage.wulu.zone` 跨域读取；这不是下载鉴权，固件文件为公开资源。设备必须直接使用 HTTPS 文件地址，不依赖 HTTP 重定向。

### 主机防护与维护

UFW 默认拒绝入站，只允许 TCP 22、80、443，覆盖 IPv4 和 IPv6；22 端口启用连接限速。80 端口供证书验证与 HTTPS 重定向使用。Caddy 只启用 HTTP/1.1 和 HTTP/2，管理接口仅监听 `127.0.0.1:2019`。

SSH 禁止 root 和空密码登录，登录等待为 30 秒，关闭 X11 转发。保留 `ubuntu` 密码登录，避免在尚未验证个人密钥登录时丢失管理入口。Fail2ban 使用 systemd 日志，10 分钟内 6 次失败后封禁 1 小时。后续如改为仅密钥登录，须先在第二个独立连接中验证用户自己的密钥，再关闭密码认证。

系统已启用每日自动更新。原镜像将 Ubuntu 安全源注释掉，本次通过独立配置补充 `Ubuntu:resolute-security`，并纳入 Caddy 官方签名稳定源 `cloudsmith/caddy/stable:any-version`；保留系统原有重启策略。核对命令为 `sudo unattended-upgrade --dry-run --debug`，不能仅以定时器已启用判定安全更新源正确。

2026-10-17 已按用户授权重启，运行内核由 `7.0.0-30-generic` 更新为 `7.0.0-38-generic`，启动标识已改变，系统不再提示需要重启。Caddy、UFW、Fail2ban 均随开机自动启动，防火墙和下载目录隔离已复验；重启后的 HTTPS、清单、HEAD、Range、404 及拒绝写入检查通过；应用和全量镜像均从公网完整下载，与发布附件逐字节一致。

本次未核查腾讯云控制台的防火墙、流量告警或 DDoS 防护配置。主机防火墙和 Fail2ban 不构成流量攻击防护或不可入侵保证。

### 原 SSH 自动部署（保留配置）

GitHub Actions 恢复后，`Website build` 在正式部署时自动上传腾讯云。发布 Release、main 的网站输入变更或在 main 手动运行时，流程下载最高版本的非草稿发布附件，校验原始清单和镜像，制作腾讯云地址的部署副本，构建网站，再通过受限 SSH 命令上传。服务器校验完整固件后先安装带版本号的 BIN，最后原子替换镜像清单；工作流从 HTTPS 完整读回清单及所有 BIN 并逐字节比较，通过后才部署 Cloudflare Pages。任一步失败都会停止后续步骤；已经上传的版本文件保留。官网部署后的标识及清单读回检查保持不变，PR 检查不接触部署密钥或服务器。

部署使用仓库 Secrets `TENCENT_DEPLOY_KEY` 和 `TENCENT_KNOWN_HOSTS`。前者是独立 Ed25519 密钥，后者是通过已认证管理员连接读取的服务器主机公钥；工作流启用严格主机密钥检查，不在运行时自动信任 `ssh-keyscan` 结果。更换主机密钥时须先独立核对，再更新 Secret。

`pico-deploy` 没有 sudo 权限，密码登录禁用；SSH 强制执行 `/usr/bin/python3 -I /usr/local/lib/pico-page/deploy_firmware.py`，由 timeout 限制为 600 秒，不允许交互终端、端口转发或用户启动脚本。配置为 `/etc/ssh/sshd_config.d/10-pico-page-deploy.conf`，授权公钥为 root 管理的 `/etc/ssh/authorized_keys/pico-deploy`。程序及复用的 `ota_manifest.py` 均由 root 管理，账号不能修改。变更这两个程序时，需要管理员先安装审核后的版本；CI 不更新服务器执行程序或主机配置。

接收程序只接受最多 9 MiB 的非压缩 tar、最多三个普通文件，文件名必须是清单和该版本的项目 BIN，不接受目录、链接、路径穿越或重复项。它复用发布校验器核对版本、大小、SHA-256、ESP 镜像结构及全量镜像中的应用；使用互斥锁串行提交，禁止用不同内容覆盖已有版本。相同附件可以重复部署，旧版本不会自动删除；空间不足时本轮失败，需要管理员清理已确认不再使用的版本。限制降低密钥泄漏的影响，但泄漏仍可能发布伪造固件；发现泄漏应立即移除服务器授权公钥并轮换 Secret。

撤回版本仍由管理员将 Release 改为草稿或删除后，在 main 手动运行工作流；程序允许清单回退到当前最高的剩余发布，保留所有既有 BIN。GitHub Release 事件使用标签中的工作流，旧标签不含本自动上传步骤时，需在 main 手动运行一次。自动部署不创建或编译新的固件版本，也不执行设备刷写。

### 原服务器手动上传

以下流程保留为故障恢复入口。除本镜像外，还须按[网站手动发布步骤](../web/README.md#github-actions-不可用时)更新官网入口。

1. 从已发布的 GitHub Release 获取清单、应用镜像、全量镜像和来源记录，不在服务器重新编译固件。
2. 在私有暂存目录保留原附件，制作镜像专用副本，只将清单 `url` 改为 `https://oc.wulu.zone/firmware/` 下的同名应用镜像。版本、镜像大小、SHA-256、数据格式版本和更新日志保持不变。
3. 在仓库根目录运行 `python3 tools/ota_manifest.py --check <镜像目录> --base-url https://oc.wulu.zone/firmware/ --expect-version <版本>`，同时核对来源记录及原发布附件。
4. 经 SSH 上传至非公开暂存目录，由管理员先安装带版本号的两个 BIN，再读回核对。文件先写入同目录临时名称，完成后重命名；不要覆盖已发布版本的 BIN。
5. 两个镜像就绪后，最后原子替换清单。公开目录的临时文件名不得符合下载允许规则。
6. 从公网完整下载两个 BIN，与发布附件逐字节比较，并核对清单、证书、HEAD、Range、缺失文件及写入请求。缺失路径应返回 404，POST、PUT、DELETE、TRACE 应返回 405。
7. 完成上述核验后，按网站手动发布步骤修改官网部署副本的清单地址，再部署官网；仅上传本服务器不会改变设备下载地址。自动部署使用上文的受限账号完成对应上传与读回。证书的真机兼容性、OTA 完整流程及同条件速度须另行验证。

安装与维护参考：[Caddy 官方安装说明](https://caddyserver.com/docs/install)、[自动 HTTPS](https://caddyserver.com/docs/automatic-https)、[Ubuntu 自动更新说明](https://ubuntu.com/server/docs/how-to/software/automatic-updates/)。
