# 腾讯云固件下载

## 下载源与职责

正式固件下载源为广州 COS：`https://pico-page-firmware-test-1258873179.cos.ap-guangzhou.myqcloud.com/`。桶只存放已发布、带版本号的 BIN，采用单 AZ、标准存储和按量计费；公有读私有写，对象继承桶 ACL，不设置匿名允许策略。未接入 CDN、全球加速或自定义域名。旧 `oc.wulu.zone` 镜像仅作人工恢复入口，不再由工作流自动更新。

双 4 MiB 设备从 `https://picopage.wulu.zone/firmware/manifest.json` 读取固定的 0.6.2 清单。双 7 MiB 设备从 `https://picopage.wulu.zone/firmware/ota-v2/manifest.json` 读取兼容版本。官网部署副本只将 `url` 改为 COS 应用镜像地址，保留 GitHub Release 原附件。双 7 MiB 版本上线前，安装页提供 0.6.2 全量镜像。上线后，安装页提供双 7 MiB 全量镜像。安装器在擦除前核对镜像 SHA-256。网站页面、两个清单及各自的镜像副本由 Cloudflare Pages 提供。

## 访问与费用边界

CORS 仅允许 `https://picopage.wulu.zone` 的 GET/HEAD，请求头允许 `Range`，暴露 `ETag`、`Content-Length`、`Content-Range` 和 `Accept-Ranges`，Max-Age 为 600 秒并返回 Vary。Referer 白名单为 `picopage.wulu.zone`，允许空 Referer 以兼容设备。CORS 不限制非浏览器客户端，Referer 可以伪造，均不替代访问控制或流量封禁。

预算告警、资源包和异常流量检测不构成实时费用硬上限。资源包超量仍按量计费，封禁前的流量仍计费；具体计费规则见[预算管理](https://cloud.tencent.com/document/product/555/65784)及[COS 计费计量说明](https://cloud.tencent.com/document/product/436/30747)。账户预算、通知接收人、实例标识和控制台操作记录不在公开文档中维护。

### 自动封禁

[`tools/cos_guard.py`](../../tools/cos_guard.py) 读取 `QCE/COS` 的 `InternetTraffic`，使用 300 秒统计、截止当前前 2 分钟的 30 分钟窗口，允许监控 API 将起点按周期向前取整。任一样本超过 100 MiB，或数据缺失、无效、超过 15 分钟未更新及查询失败时，将指定桶 ACL 改为 `private` 并保留错误供监控。按每分钟调度设计，仍受采集、调度、执行延迟及平台故障影响，不能保证费用封顶。

guard 是否启用以云端实际配置为准，源码检查不能证明云端当前状态；启用前须排查监控异常并明确缺失数据的处理策略。guard 不会自动恢复公开访问；恢复前须确认异常原因并由管理员操作。桶内对象必须继承桶权限，不得单独公开对象或添加匿名读取策略，否则桶 ACL 封禁可能失效。封禁也不能撤销已签发且仍有效的授权链接。

运行角色按 [`tools/cos-guard-policy.json`](../../tools/cos-guard-policy.json) 限定为监控读取和目标桶的 `cos:PutBucketACL`，不授予上传、删除或其他桶的写权限；SCF 服务配置角色与函数运行角色分开。函数从调用上下文的 `environment` JSON 或兼容顶层字段取得临时凭据，不保存长期密钥，不接受事件传入的桶名、阈值或恢复指令。

函数包使用 Python 3.10、Linux x86-64 依赖，版本固定于 [`tools/cos-requirements.txt`](../../tools/cos-requirements.txt)。SDK 和传递依赖不进入固件或网站；打包时保留许可证、元数据和来源记录，不使用其他平台的二进制扩展。`crcmod` 使用上游支持的纯 Python 回退。重新部署时分别验证依赖导入、凭据解析、监控读取、封禁和人工恢复；函数测试不替代设备 OTA 验收。

## 发布身份与最小权限

[`tools/publish_cos.py`](../../tools/publish_cos.py) 复用发布校验器，只上传带版本号的 BIN，继承桶 ACL，使用服务端禁止覆盖请求头；完整认证读回一致后才允许后续网站部署，不会将桶重新公开。COS 不需要可变的 `manifest.json`。

上传角色通过 GitHub OIDC 换取临时凭据。权限以 [`tools/cos-publisher-policy.json`](../../tools/cos-publisher-policy.json) 为模板，仅允许目标桶项目 BIN 的读取和禁止覆盖上传，不含删除或 ACL 修改。信任策略见 [`tools/cos-publisher-trust.json`](../../tools/cos-publisher-trust.json)，限定当前仓库不可变 OIDC 主体的 main 与发布标签，拒绝 PR、其他分支和其他仓库；issuer、audience 也须匹配。复用模板时核对账户、桶及仓库身份，不原样沿用本项目实例标识。仓库重建或迁移后，仓库 ID 会变化，须重新核对信任条件。

脚本请求 900 秒会话；该请求时长不等于云端角色最大会话期限。角色、提供商和策略的实际绑定须由维护者核对，公开模板不证明已部署。桶名、角色 ID 等资源标识不是密码；凭据和令牌不得写入文档或构建日志。

## 自动部署

单独修改发布说明时，main 不部署网站。发布 Release 后，工作流读取已公开版本的说明和固件。

`Website build` 在 main 网站输入变化、main 手动运行或发布 Release 时部署。工作流分别取回固定的 0.6.2 和高于 0.6.2 的最高非草稿版本，逐个核对布局、版本、应用和全量镜像。校验通过后改写部署清单的 COS 地址并构建网站。通过 GitHub OIDC 换取独立发布角色的临时凭据，使用服务端 `x-cos-forbid-overwrite: true` 上传带版本号的 BIN。发布前先认证读回已有对象并逐字节比较，只有内容完全相同时才复用。服务端明确返回 `NoSuchKey` 时上传新对象。上传使用 SDK 文件流分块发送，单次套接字操作超时为 30 秒，保留禁止覆盖请求头、MD5 和完整认证读回。最后通过匿名 HTTPS 完整读回并比较。COS 返回 HTTP 403 时，每个受影响渠道的清单和两个镜像须与官网现有副本逐字节一致，才允许继续更新静态页面并在 Actions 告警。认证上传、读回或内容校验失败均停止部署。

上述核对通过后部署 Cloudflare Pages，并读回正式域名的部署标识及两个清单。PR 只构建和验证。角色信任限于 main 和发布标签。回退使用支持双渠道的站点产物，保留已上线的新布局清单和安装包。旧标签使用旧工作流时，在 main 手动运行最新流程，避免重跑历史发布流程。

### 首次上传链路过慢时的恢复

GitHub 到 COS 的新镜像上传若返回 `UserNetworkTooSlow`，可在已获发布授权的范围内从本机控制台导入同一正式镜像，再由原工作流完成认证核验和网站部署。已有对象复用成功不代表首次上传链路已恢复。

1. 从对应干净标签构建并核验正式附件，或下载正式 Release 附件，确认应用与全量镜像的版本、长度及 SHA-256。
2. 管理员登录腾讯云控制台，在同一桶根目录上传这两个带版本号的 BIN，选择标准存储并继承桶权限。同名对象已存在时先完整读回比较，只复用一致内容，不覆盖不同内容；不上传可变清单或修改 ACL。
3. 完整匿名读回两镜像并与正式附件比较，核对官网来源 CORS 后，在 main 手动运行 `Website build`。等待认证与公开读回、Pages 部署及生产检查全部成功，再独立核对官网清单、安装页版本／摘要、更新日志和部署标识。
