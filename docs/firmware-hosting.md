# 腾讯云固件下载镜像

## 当前范围

`https://oc.wulu.zone/firmware/` 是独立的 HTTPS 下载镜像，2026-10-17 部署于 Ubuntu 26.04 LTS，使用 Caddy 2.11.7。当前清单及对应应用、全量镜像为 `0.2.0-beta.6`，2026-10-17 手动更新；保留 beta.5 的版本文件。两个 BIN 与 GitHub Release 附件逐字节相同；镜像清单只修改 `url`，指向本域名下的应用镜像，其余字段沿用发布附件。

设备仍从 `https://picopage.wulu.zone/firmware/manifest.json` 读取清单，官网及网站发布工作流保持原状。镜像部署没有切换设备下载源，也没有实现地区分流或后续版本自动上传。当前阶段不能宣称真机 OTA 或中国大陆下载提速已验收。

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

下载目录属主为 `root:caddy`，目录权限为 `0750`，文件权限为 `0640`。Caddy 以专用用户运行，服务内的 `/srv/pico-page` 额外挂载为只读；启用 `NoNewPrivileges` 和 `ProtectHome`，仅保留 `CAP_NET_BIND_SERVICE`。证书状态由 Caddy 管理，不放入下载目录。

站点只允许 GET 和 HEAD，只提供 `/firmware/manifest.json` 及符合项目命名规则的应用和全量 BIN。其他路径返回 404，其他请求方法返回 405。不开启目录列表、上传、动态脚本或反向代理；不要将符号链接、仓库、凭据、日志或备份放入下载根目录。

清单使用 `Cache-Control: no-store`。文件响应允许官网 `https://picopage.wulu.zone` 跨域读取；这不是下载鉴权，固件文件为公开资源。设备必须直接使用 HTTPS 文件地址，不依赖 HTTP 重定向。

## 主机防护与维护

UFW 默认拒绝入站，只允许 TCP 22、80、443，覆盖 IPv4 和 IPv6；22 端口启用连接限速。80 端口供证书验证与 HTTPS 重定向使用。Caddy 只启用 HTTP/1.1 和 HTTP/2，管理接口仅监听 `127.0.0.1:2019`。

SSH 禁止 root 和空密码登录，登录等待为 30 秒，关闭 X11 转发。保留 `ubuntu` 密码登录，避免在尚未验证个人密钥登录时丢失管理入口。Fail2ban 使用 systemd 日志，10 分钟内 6 次失败后封禁 1 小时。后续如改为仅密钥登录，须先在第二个独立连接中验证用户自己的密钥，再关闭密码认证。

系统已启用每日自动更新。原镜像将 Ubuntu 安全源注释掉，本次通过独立配置补充 `Ubuntu:resolute-security`，并纳入 Caddy 官方签名稳定源 `cloudsmith/caddy/stable:any-version`；保留系统原有重启策略。核对命令为 `sudo unattended-upgrade --dry-run --debug`，不能仅以定时器已启用判定安全更新源正确。

2026-10-17 安装系统更新后，当前运行内核仍为 `7.0.0-30-generic`，已安装的新内核为 `7.0.0-38-generic`。系统明确要求重启，本次为保留用户管理连接未执行重启。维护人员应安排重启，随后重新核对服务、UFW、Fail2ban、HTTPS 和镜像摘要。

本次未核查腾讯云控制台的防火墙、流量告警或 DDoS 防护配置。主机防火墙和 Fail2ban 不构成流量攻击防护或不可入侵保证。

## 后续上传与切换

2026-10-17 用户确认 GitHub Actions 整体暂不可用，下一版先手动上传，不接入自动部署，不新增长期 CI 登录凭据。除本镜像外，还须按[网站手动发布步骤](../web/README.md#github-actions-不可用时)更新原官网入口；不能依赖创建 Release 自动完成网站部署。

1. 从已发布的 GitHub Release 获取清单、应用镜像、全量镜像和来源记录，不在服务器重新编译固件。
2. 在私有暂存目录保留原附件，制作镜像专用副本，只将清单 `url` 改为 `https://oc.wulu.zone/firmware/` 下的同名应用镜像。版本、镜像大小、SHA-256、数据格式版本和更新日志保持不变。
3. 在仓库根目录运行 `python3 tools/ota_manifest.py --check <镜像目录> --base-url https://oc.wulu.zone/firmware/ --expect-version <版本>`，同时核对来源记录及原发布附件。
4. 经 SSH 上传至非公开暂存目录，由管理员先安装带版本号的两个 BIN，再读回核对。文件先写入同目录临时名称，完成后重命名；不要覆盖已发布版本的 BIN。
5. 两个镜像就绪后，最后原子替换清单。公开目录的临时文件名不得符合下载允许规则。
6. 从公网完整下载两个 BIN，与发布附件逐字节比较，并核对清单、证书、HEAD、Range、缺失文件及写入请求。缺失路径应返回 404，POST、PUT、DELETE、TRACE 应返回 405。
7. 正式接入须另行调整现有清单和发布工作流，并验证已发布固件的证书兼容性及真机 OTA。仅在本镜像上传文件，不会改变设备使用的固定清单入口。

安装与维护参考：[Caddy 官方安装说明](https://caddyserver.com/docs/install)、[自动 HTTPS](https://caddyserver.com/docs/automatic-https)、[Ubuntu 自动更新说明](https://ubuntu.com/server/docs/how-to/software/automatic-updates/)。
