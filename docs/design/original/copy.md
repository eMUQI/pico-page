# 界面文案

`{}` 内为运行时字段。示例值不可硬编码。文案按统一状态文案表（交接说明 §9）校对过。

## 通用

| 键 | 文案 |
|---|---|
| back_library | ‹ 书库 |
| back_tools | ‹ 工具 |
| back_settings | ‹ 设置 |
| collapse | 收起 |
| cancel | 取消 |
| close | 关闭 |
| retry | 重试 |
| unknown | 未知 |
| needs_network | 需联网 |
| needs_storage | 需存储卡 |
| display_busy | 屏幕正在刷新，完成后可用 |

## 阅读页 S02

| 键 | 文案 |
|---|---|
| footer_chapter | 本章 {page} / {total} |
| footer_chapter_unknown | 本章第 {page} 屏 |
| footer_book | 全书 {percent}% |
| preparing_chapter | 正在准备这一章 |
| preparing_restore | 将回到上次保存的位置 |
| image_unavailable | 图片暂无法显示，正文仍可阅读 |
| image_reason_size | 原因：图片尺寸超出设备能力 |
| return_origin | ‹ 返回原位置 |
| return_before_jump | ‹ 返回跳转前位置 |
| finished_title | 已读完《{title}》 |
| finished_back | 返回书库 |
| finished_toc | 查看目录 |
| chapter_load_fail | 下一章无法打开 |
| chapter_load_fail_body | 内容文件损坏或缺失。上一页仍可阅读，阅读位置没有改变。 |
| stay_here | 留在此页 |
| open_toc | 打开目录 |
| storage_removed | 存储卡已移除，无法加载后续内容 |
| storage_removed_body | 插回后会重新核对这本书。 |
| not_saved | 此处尚未保存 |
| font_fallback | 字体不可用，已使用替代字体 |

## 工具面板 S03 / 标签

| 键 | 文案 |
|---|---|
| tab_toc / tab_typo / tab_sync / tab_refresh / tool_full_refresh | 目录 / 排版 / 同步 / 刷新 / 全刷 |
| chip_pending | ⇅ 待上传 |
| chip_uploaded | ⇅ 已上传 |
| chip_syncing | ⇅ 同步中 |
| chip_unconfigured | ⇅ 未配置 |
| chip_conflict | 位置不同 › |
| chip_auth | 登录失效 › |
| chip_mapping | ⇅ 未能对应 |
| chip_mismatch | ⇅ 暂未同步 |
| chip_not_saved | 尚未保存 |
| sync_needs_choice | 待选择 |
| panel_storage_disabled | 存储卡已移除，目录和排版暂不可用 |

## 目录 S04

| 键 | 文案 |
|---|---|
| toc_current | 当前位置 |
| toc_missing | 这本书未提供目录 |
| toc_missing_body | 下面按书中内容的顺序列出，名称由设备生成，不是原书的章节标题。 |
| toc_fallback_item | 第 {n} 部分 · 无标题 |
| toc_loading | 正在读取目录 |
| toc_loading_body | 可以收起面板继续阅读 |
| toc_jump_fail | 无法跳转到“{label}”，已留在当前位置 |
| toc_jump_fail_row | 无法打开 |
| back_to_text | 回到正文 |
| pager_prev / pager_next | ‹ 上一屏 / 下一屏 › |

## 排版 S05

| 键 | 文案 |
|---|---|
| typo_scope | 作用域：此书 |
| typo_font / size / line / margin | 字体 / 字号 / 行距 / 页边距 |
| typo_advanced | 段距、缩进 ▸ |
| typo_basic | ‹ 基本 |
| typo_para / indent | 段距 / 首行缩进 |
| typo_reset | 恢复默认 |
| typo_set_default | 设为阅读默认 |
| typo_apply | 应用 |
| typo_applied | 已应用 |
| typo_tag_current | 当前排版 |
| typo_tag_preview | 预览 · 尚未应用 |
| typo_reflowing | 正在重新排版 |
| typo_reflow_body | 正在按新设置排版全书 |
| typo_reflow_note | 完成后停在原文附近；期间可以继续阅读当前屏。 |
| typo_cancel_reflow | 取消，恢复原排版 |
| typo_reflow_busy | 重排中 |
| typo_font_unavailable | 字体不可用 |
| typo_cannot_apply | 无法应用 |

## 同步 S06 / 冲突 S07

| 键 | 文案 |
|---|---|
| sync_col_local / server / other | 本机 / 服务器 / 其他设备 |
| sync_saved | 已保存 |
| sync_saved_offline | 此处已保存，联网后可同步 |
| sync_offline | 暂时无法连接，不影响继续阅读 |
| sync_uploaded | 已上传此处 |
| sync_uploaded_note | 已上传此处。另一台设备需要自己同步后才会看到这个位置。 |
| sync_last_ack | 最近一次上传确认：{time} |
| sync_check | 检查并同步 |
| sync_pull | 从其他设备接续 |
| sync_push | 将此处上传 |
| sync_busy | 同步中 |
| sync_cancel | 取消同步 |
| sync_stage | 第 {n} 步 / 共 {m} 步 |
| sync_stage_note | 先读取远端位置，再决定是否上传。期间可以收起面板继续阅读。 |
| sync_unconfigured | 尚未设置 KOReader 同步。服务地址和账户需要在手机或电脑上填写，设备负责发起和确认。 |
| sync_setup | 设置同步 |
| sync_auth_fail | 同步登录已失效，请重新配置 |
| sync_auth_fail_body | 网络正常。待上传的位置会保留，配置后再上传。 |
| sync_reconfigure | 重新配置 |
| sync_mapping_fail | 无法准确对应此位置，已保留当前阅读位置 |
| sync_mapping_body | 另一设备的位置在本机无法解析，只能定位到附近。 |
| sync_keep | 保留此处 |
| sync_nearby | 转到附近（约 {percent}%） |
| sync_nearby_note | 附近定位为实验功能，跳转后可退回。 |
| sync_mismatch | 无法确认是同一本书，暂未同步 |
| sync_mismatch_body | 本机文件与服务器记录的标识不一致。不会按同名文件覆盖。 |
| conflict_title | 发现不同的阅读位置 |
| conflict_body | 两台设备的阅读位置不同，请选择继续位置 |
| conflict_body_approx | 另一设备的位置信息有限，只能定位到附近 |
| conflict_this | 此设备 |
| conflict_other | 另一设备 |
| conflict_local_saved | 本机已保存 |
| conflict_from | 来自 KOReader |
| conflict_received | 最近收到：{time} |
| conflict_received_local | 本机收到：{time} |
| conflict_time_unknown | 阅读时间未知 |
| conflict_chapter_unknown | 章节无法确认 |
| conflict_keep | 继续此处 |
| conflict_go | 转到另一设备的位置 |
| conflict_go_near | 转到附近位置 |
| conflict_keep_note | “继续此处”只关闭提示，服务器上的位置保持不变。 |
| conflict_replace | 上传此处，替换远端位置 |
| conflict_replace_note | 需再次确认，会覆盖服务器记录 |
| conflict_going | 正在转到另一设备的位置 |
| conflict_going_busy | 跳转中 |
| replace_confirm_title | 上传此处，替换远端位置？ |
| replace_confirm_body | 服务器上的“{remote}”将改为此处“{local}”。 |
| replace_confirm_note | 另一台设备下次同步时才会看到这个位置。这个操作不会删除另一设备上的阅读记录。 |
| replace_confirm_ok | 上传并替换 |

## 刷新 S08

| 键 | 文案 |
|---|---|
| preset_clear | 清晰 · 优先文字与图像表现 |
| preset_balanced | 均衡 · 在显示效果和操作响应之间折中 |
| preset_fast | 快速 · 优先操作响应，可能减少灰阶或更易留痕 |
| preset_current | 当前 |
| full_refresh | 立即全刷 |
| full_refresh_note | 整屏刷新一次，用于清理残影 |
| auto_full_refresh | 自动全刷间隔 |
| fast_confirm_title | 改用快速？ |
| fast_confirm_body | 翻页和面板响应会更快，但画面可能灰阶更少，也更容易留下残影。觉得画面变脏时，可以用“全刷”清理。 |
| fast_keep / fast_ok | 保持均衡 / 改用快速 |
| refresh_fail | 屏幕刷新未完成 |
| refresh_fail_body | 已保留上一画面，阅读位置没有改变。可以再试一次全刷；反复出现时，可以在设置里导出诊断信息。 |
| refresh_retry | 再试一次 |

## 书库 S01

| 键 | 文案 |
|---|---|
| lib_title | 书库 |
| lib_import / lib_settings | 导入 / 设置 |
| lib_continue | 继续阅读 |
| lib_continue_go | 继续 › |
| lib_count | 本机书籍 · {n} 本 |
| lib_sort_recent | 最近阅读 ▾ |
| lib_view_list / card | 列表 / 卡片 |
| lib_unread / finished | 未读 / 已读完 |
| lib_empty | 书库里还没有书 |
| lib_empty_ready | 存储卡已就绪 · 0 本 |
| lib_way_copy | 复制到存储卡 |
| lib_way_copy_body | 在电脑上用预处理工具处理 EPUB，把结果复制到存储卡的 books 文件夹，再插回设备。 |
| lib_way_transfer | 通过局域网传输 |
| lib_way_transfer_body | 在设备上开启传输，再用手机或电脑发送已处理的内容。 |
| lib_no_raw_epub | 设备不能直接打开未经处理的 EPUB 文件。 |
| lib_rescan | 重新扫描 |
| lib_start_transfer | 开启传输 |
| lib_scanning | 正在扫描存储卡 |
| lib_scanning_found | 已找到 {n} 本 |
| lib_scanning_partial | 本机书籍 · 扫描完成前列表可能不完整 |
| lib_storage_unavailable | 存储卡不可用 |
| lib_storage_unavailable_body | 未检测到存储卡，无法显示书库。这不代表书已被删除。 |
| lib_storage_hint | 请确认存储卡已插好。插回后会按内容重新核对书籍，不会仅凭文件路径沿用旧进度。 |
| lib_last_read | 上次阅读 |
| lib_last_read_note | 位置已保存在本机，插回存储卡后可继续 |
| lib_redetect | 重新检测 |
| lib_unavailable_count | {n} 本无法打开 |
| lib_cleanup | 清理缺失书目 › |
| item_missing | 文件缺失，可能已被移动或删除 |
| item_reprocess | 需要重新处理：阅读包版本过旧 |
| item_corrupt | 内容已损坏 |
| cleanup_title | 从书库移除 {n} 本已缺失的书？ |
| cleanup_body | 只移除书库里的条目，阅读位置记录保留，放回同一本书后可以接着读。 |
| cleanup_note | “需要重新处理”的 {n} 本不在此列。服务器上的同步记录不受影响。 |
| cleanup_ok | 移除条目 |
| detail_progress / sync / size | 进度 / 同步 / 占用 |
| detail_manage | 本机管理 · 都不会改动服务器上的同步记录 |
| detail_clear_cache | 清理缓存 · 可重建，不影响书和进度 |
| detail_clear_progress | 清除本机进度 · 书保留，从头开始 |
| detail_delete | 从本机删除书籍 · 删除存储卡上的这本书 |
| detail_missing_title | 文件缺失 |
| detail_missing_body | 存储卡上找不到这本书，可能已被移动、改名或删除。 |
| detail_missing_hint | 放回存储卡后重新扫描，设备会按内容认出这本书，即使换了文件夹也能接着读。 |
| detail_remove_entry | 从书库移除条目 · 保留阅读位置记录 |
| detail_remove_entry_progress | 移除条目并清除进度 · 放回后从头开始 |
| delete_title | 从本机删除这本书？ |
| delete_body | 会删除存储卡上的书和缓存，共 {size}。 |
| delete_note | 本机阅读位置保留，重新导入同一本书后可以接着读。服务器上的同步记录不受影响。 |
| delete_ok | 删除 |
| deleted_toast | 已从本机删除 |

## 连接与导入 S09

| 键 | 文案 |
|---|---|
| import_title | 导入 |
| import_way_transfer_body | 从手机或电脑发送已处理的内容。设备和发送端需要在同一个 Wi-Fi 下。 |
| import_device_only_processed | 设备只接收处理过的内容，不能在本机转换原始 EPUB。 |
| wifi_connect | 连接 Wi-Fi |
| wifi_not_connected | 未连接 |
| wifi_step1 | 手机连接设备热点 |
| wifi_step2 | 扫码打开配置页，选择网络并填写密码 |
| wifi_only_24 | 只支持 2.4 GHz 网络，配置页只列出 2.4 GHz 网络 |
| wifi_wait_phone | 等待手机连接 |
| wifi_input_on_device | 在设备上输入 |
| wifi_fail_title | 无法连接到“{ssid}” |
| wifi_fail_5g | 这可能是 5 GHz 网络。设备只支持 2.4 GHz Wi-Fi。 |
| wifi_fail_5g_hint | 请改选同一路由器的 2.4 GHz 网络，或在路由器上开启 2.4 GHz。设备没有保存这次的设置。 |
| wifi_fail_password | 密码不正确。设备没有保存这次的设置。 |
| wifi_refill | 重新填写 |
| transfer_title | 传输中 |
| transfer_end | 结束传输 |
| transfer_hint | 在手机或电脑的浏览器里打开下面的地址，选择已处理的文件发送。 |
| transfer_code | 访问码 {code} |
| transfer_waiting | 等待发送 |
| transfer_note | 访问码只在本次传输有效，结束后服务立即关闭。传输期间设备不会自动休眠。 |
| transfer_summary | 本次 {n} 个文件 · 已加入 {a} · 失败 {f} |
| transfer_stage_receive / verify / add | 接收文件 / 校验内容 / 加入书库 |
| transfer_added | 已加入 |
| transfer_err_raw_epub | 校验未通过：这是原始 EPUB，请先在电脑上处理 |
| transfer_remove | 移除 |
| transfer_end_title | 结束传输？ |
| transfer_end_body | 退出将停止传输。“{name}”还没接收完，不会加入书库。 |
| transfer_end_writing | “{name}”正在写入，会先完成收尾再关闭，大约需要几秒。 |
| transfer_continue | 继续传输 |
| transfer_end_ok | 结束 |

## 设置 S10

| 键 | 文案 |
|---|---|
| set_network | 网络与传输 |
| set_kosync | KOReader 同步 |
| set_display | 显示与刷新 |
| set_input | 输入与阅读 |
| set_power | 电源与休眠 |
| set_storage | 存储与设备 |
| kosync_server / account / credential | 服务地址 / 账户 / 凭据 |
| kosync_hidden | 已隐藏 |
| kosync_edit_on_phone | 在手机上修改地址或账户 |
| kosync_check_on_open | 打开书时检查远端位置 · 只在已连网时进行，不会自动联网 |
| kosync_push_before_sleep | 休眠前上传此处 · 有时限，失败不影响休眠 |
| kosync_match | 书籍匹配方式 · 按文件名是兼容选项，同名不同书可能混淆 |
| kosync_match_content | 按内容 |
| kosync_logout | 退出同步账户 · 清除本机凭据，不会删除服务器上的账户 |
| storage_books / cache / free | 书籍原文件 / 缓存（可重建） / 可用空间 |
| storage_auto_remove | 自动移除缺失书目 · 阅读位置记录仍保留 |
| device_fw / hw | 固件版本 / 硬件 |
| device_export_diag | 导出诊断信息 · 写入存储卡，不含账户凭据 |
| storage_clear_cache | 清理全部缓存 · 不删除书和阅读进度，下次打开会稍慢 |
| toggle_on / off | 开 / 关 |
| set_save_fail | 设置未保存 |
| set_save_fail_body | “{name}”写入失败，仍按原设置（{value}）生效。请检查存储卡。 |
| set_discard | 放弃修改 |
| clock_unavailable | 暂不可用：设备时间尚未校准 |

## 休眠 S11

| 键 | 文案 |
|---|---|
| sleep_title | 已休眠 |
| sleep_wake_hint | 轻触中间按键唤醒（占位，以板级确认为准） |
| sleep_pending | 此处已保存 / 尚未上传 |
| sleep_restoring | 正在恢复 |
| sleep_restoring_body | 回到本机保存的位置，暂时无法翻页 |
| sleep_low_battery | 电量低，已休眠 |
| sleep_low_battery_body | 剩余 {percent}%。阅读位置已保存在本机，尚未上传。请充电后再唤醒。 |

## 通用提示 S12

| 键 | 文案 |
|---|---|
| toast_dismiss | 轻触关闭 |
| save_fail_title | 此处尚未保存，请检查存储 |
| save_fail_body | 写入存储卡失败。当前页仍可阅读，翻页后会再尝试保存。 |
| book_corrupt_title | 这本书无法打开 |
| book_corrupt_body | 阅读包已损坏，需要在电脑上重新处理后再导入。本机阅读位置已保留。 |
| export_diag | 导出诊断信息 |
| clear_progress_title | 清除本机进度？ |
| clear_progress_body | 《{title}》会从头开始。书和缓存保留。服务器上的位置不变，下次同步时会提示两处位置不同。 |
| clear_progress_ok | 清除进度 |
