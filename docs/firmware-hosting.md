# 腾讯云固件下载

## 当前范围

`https://oc.wulu.zone/firmware/` 是独立的 HTTPS 下载镜像，2026-10-17 部署于 Ubuntu 26.04 LTS，使用 Caddy 2.11.7。当前清单及对应应用、全量镜像为 `0.2.0-beta.6`，2026-10-17 手动更新；保留 beta.5 的版本文件。两个 BIN 与 GitHub Release 附件逐字节相同；镜像清单只修改 `url`，指向本域名下的应用镜像，其余字段沿用发布附件。

设备仍从 `https://picopage.wulu.zone/firmware/manifest.json` 读取清单；官网部署副本将清单的 `url` 指向 `https://oc.wulu.zone/firmware/` 下的同名应用镜像。固定清单入口不变，已安装固件无需重新烧录。GitHub Release 的原附件保留，官网部署前先校验附件，再用 `--publish-base-url https://oc.wulu.zone/firmware/` 只修改清单地址。网站安装页的全量镜像仍从官网提供。自动上传流程见下文；地区分流未实现，真机 OTA 与下载性能结果见开发状态对应记录。

## 文件与访问边界

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

## 主机防护与维护

UFW 默认拒绝入站，只允许 TCP 22、80、443，覆盖 IPv4 和 IPv6；22 端口启用连接限速。80 端口供证书验证与 HTTPS 重定向使用。Caddy 只启用 HTTP/1.1 和 HTTP/2，管理接口仅监听 `127.0.0.1:2019`。

SSH 禁止 root 和空密码登录，登录等待为 30 秒，关闭 X11 转发。保留 `ubuntu` 密码登录，避免在尚未验证个人密钥登录时丢失管理入口。Fail2ban 使用 systemd 日志，10 分钟内 6 次失败后封禁 1 小时。后续如改为仅密钥登录，须先在第二个独立连接中验证用户自己的密钥，再关闭密码认证。

系统已启用每日自动更新。原镜像将 Ubuntu 安全源注释掉，本次通过独立配置补充 `Ubuntu:resolute-security`，并纳入 Caddy 官方签名稳定源 `cloudsmith/caddy/stable:any-version`；保留系统原有重启策略。核对命令为 `sudo unattended-upgrade --dry-run --debug`，不能仅以定时器已启用判定安全更新源正确。

2026-10-17 已按用户授权重启，运行内核由 `7.0.0-30-generic` 更新为 `7.0.0-38-generic`，启动标识已改变，系统不再提示需要重启。Caddy、UFW、Fail2ban 均随开机自动启动，防火墙和下载目录隔离已复验；重启后的 HTTPS、清单、HEAD、Range、404 及拒绝写入检查通过；应用和全量镜像均从公网完整下载，与发布附件逐字节一致。

本次未核查腾讯云控制台的防火墙、流量告警或 DDoS 防护配置。主机防火墙和 Fail2ban 不构成流量攻击防护或不可入侵保证。

## 自动部署

GitHub Actions 恢复后，`Website build` 在正式部署时自动上传腾讯云。发布 Release、main 的网站输入变更或在 main 手动运行时，流程下载最高版本的非草稿发布附件，校验原始清单和镜像，制作腾讯云地址的部署副本，构建网站，再通过受限 SSH 命令上传。服务器校验完整固件后先安装带版本号的 BIN，最后原子替换镜像清单；工作流从 HTTPS 完整读回清单及所有 BIN 并逐字节比较，通过后才部署 Cloudflare Pages。任一步失败都会停止后续步骤；已经上传的版本文件保留。官网部署后的标识及清单读回检查保持不变，PR 检查不接触部署密钥或服务器。

部署使用仓库 Secrets `TENCENT_DEPLOY_KEY` 和 `TENCENT_KNOWN_HOSTS`。前者是独立 Ed25519 密钥，后者是通过已认证管理员连接读取的服务器主机公钥；工作流启用严格主机密钥检查，不在运行时自动信任 `ssh-keyscan` 结果。更换主机密钥时须先独立核对，再更新 Secret。

`pico-deploy` 没有 sudo 权限，密码登录禁用；SSH 强制执行 `/usr/bin/python3 -I /usr/local/lib/pico-page/deploy_firmware.py`，由 timeout 限制为 600 秒，不允许交互终端、端口转发或用户启动脚本。配置为 `/etc/ssh/sshd_config.d/10-pico-page-deploy.conf`，授权公钥为 root 管理的 `/etc/ssh/authorized_keys/pico-deploy`。程序及复用的 `ota_manifest.py` 均由 root 管理，账号不能修改。变更这两个程序时，需要管理员先安装审核后的版本；CI 不更新服务器执行程序或主机配置。

接收程序只接受最多 9 MiB 的非压缩 tar、最多三个普通文件，文件名必须是清单和该版本的项目 BIN，不接受目录、链接、路径穿越或重复项。它复用发布校验器核对版本、大小、SHA-256、ESP 镜像结构及全量镜像中的应用；使用互斥锁串行提交，禁止用不同内容覆盖已有版本。相同附件可以重复部署，旧版本不会自动删除；空间不足时本轮失败，需要管理员清理已确认不再使用的版本。限制降低密钥泄漏的影响，但泄漏仍可能发布伪造固件；发现泄漏应立即移除服务器授权公钥并轮换 Secret。

撤回版本仍由管理员将 Release 改为草稿或删除后，在 main 手动运行工作流；程序允许清单回退到当前最高的剩余发布，保留所有既有 BIN。GitHub Release 事件使用标签中的工作流，旧标签不含本自动上传步骤时，需在 main 手动运行一次。自动部署不创建或编译新的固件版本，也不执行设备刷写。

## Actions 不可用时手动上传

以下流程保留为故障恢复入口。除本镜像外，还须按[网站手动发布步骤](../web/README.md#github-actions-不可用时)更新官网入口。

1. 从已发布的 GitHub Release 获取清单、应用镜像、全量镜像和来源记录，不在服务器重新编译固件。
2. 在私有暂存目录保留原附件，制作镜像专用副本，只将清单 `url` 改为 `https://oc.wulu.zone/firmware/` 下的同名应用镜像。版本、镜像大小、SHA-256、数据格式版本和更新日志保持不变。
3. 在仓库根目录运行 `python3 tools/ota_manifest.py --check <镜像目录> --base-url https://oc.wulu.zone/firmware/ --expect-version <版本>`，同时核对来源记录及原发布附件。
4. 经 SSH 上传至非公开暂存目录，由管理员先安装带版本号的两个 BIN，再读回核对。文件先写入同目录临时名称，完成后重命名；不要覆盖已发布版本的 BIN。
5. 两个镜像就绪后，最后原子替换清单。公开目录的临时文件名不得符合下载允许规则。
6. 从公网完整下载两个 BIN，与发布附件逐字节比较，并核对清单、证书、HEAD、Range、缺失文件及写入请求。缺失路径应返回 404，POST、PUT、DELETE、TRACE 应返回 405。
7. 完成上述核验后，按网站手动发布步骤修改官网部署副本的清单地址，再部署官网；仅上传本服务器不会改变设备下载地址。自动部署使用上文的受限账号完成对应上传与读回。证书的真机兼容性、OTA 完整流程及同条件速度须另行验证。

安装与维护参考：[Caddy 官方安装说明](https://caddyserver.com/docs/install)、[自动 HTTPS](https://caddyserver.com/docs/automatic-https)、[Ubuntu 自动更新说明](https://ubuntu.com/server/docs/how-to/software/automatic-updates/)。
