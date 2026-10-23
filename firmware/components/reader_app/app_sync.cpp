#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::draw_sync_settings() {
    if (!bookmark_io_ready()) return;
    view = View::SyncSettings;
    hits.clear(); canvas.clear();
    if (account_page == AccountPage::Remove) {
        header("移除同步账户", 0);
        text("移除同步账户？", 36, 160, 34, 612, 1, true);
        wrapped("设备会删除 PicoPage/sync.toml。阅读位置仍保存在存储卡，服务器上的记录不受影响。", 36, 244, 612, 28);
        button(Back, "取消", 24, 1080, 306, 96, true);
        button(AccountRemoveConfirm, "移除", 354, 1080, 306, 96);
        return;
    }
    if (account_page == AccountPage::Verifying) {
        header("KOReader 同步", 0);
        text("正在验证账户", 36, 160, 34, 612, 1, true);
        text("网络：" + network_label(), 36, 236, 24, 612, 3);
        button(AccountCancel, "取消", 24, 1000, 636, 96);
        note("取消后不保存账户。", 1124);
        return;
    }
    header("KOReader 同步", Back, "设置");
    sync::Config next;
    const bool complete = account_config(next);
    note(!account.saved || account.changed ? "账户 · 未保存"
         : account.check == AccountDraft::Check::Rejected ? "账户 · 验证未通过"
         : account.check == AccountDraft::Check::Accepted ? "账户 · 已验证" : "账户 · 已保存", 130);
    value_row(AccountEndpoint, "服务地址", "", server_name(next), 170, 96);
    value_row(AccountUser, "账户", "", account.username.empty() ? "未填写" : account.username, 266, 96);
    value_row(AccountPassword, "密码", "", !account.password.empty() ? "已填写" : !next.userkey.empty() ? "已保存" : "未填写", 362, 96);
    wrapped("验证时需要联网。也可以在电脑上编辑 PicoPage/sync.toml。", 36, 482, 612, 22, true, 3);
    const auto verify = account.saved && !account.changed ? "重新验证" : "验证并保存账户";
    if (complete) button(AccountVerify, verify, 24, 580, 636, 96, account.changed || !account.saved);
    else disabled_button(verify, 24, 580, 636, 96);
    if (account.saved) button(AccountRemove, "移除同步账户", 24, 700, 636, 96);
    if (has_book() && account.saved && !account.changed && !txt_name(book_path))
        value_row(Sync, "检查《" + document.metadata().title + "》的同步", "", "", 828, 96);
    else if (has_book() && txt_name(book_path)) note("TXT 只保存本机进度，不支持 KOReader 接续。", 860);
    wrapped(account_notice, 36, 1040, 612, 24);
}


void ReaderApp::Impl::verify_account() {
    sync::Config next;
    sync::HttpRequest request;
    std::string problem;
    if (!account_config(next) || !sync::auth_request(next, request, problem)) {
        account_notice = "服务地址必须以 https:// 开头，账户和密码不能为空。";
    } else if (!wifi_enabled()) account_notice = "Wi-Fi 已关闭。请先在 Wi-Fi 设置中打开。";
    else if (wifi_worker || portal_live || active_network) account_notice = "请先结束当前任务。";
    else {
        auto targets = wifi_targets(problem);
        if (targets.networks.empty()) account_notice = problem;
        else {
            account_check = next;
            account_hold = std::make_shared<std::atomic_bool>(false);
            active_network = ++network_sequence;
            uploading = false;
            network_cancel = std::make_shared<std::atomic_bool>(false);
            network_request = NetworkOperation{active_network, std::move(request), network_cancel, account_hold, std::move(targets)};
            account_page = AccountPage::Verifying;
            account_notice.clear();
        }
    }
    draw_sync_settings(); overlay();
}


void ReaderApp::Impl::draw_sync() {
    if (!bookmark_io_ready()) return;
    view = View::Sync;
    if (!has_book()) {
        hits.clear(); canvas.clear(); header("同步", Back, "书库");
        note("请先打开一本 EPUB。", 246); return;
    }
    // content_loading here means that a position is being mapped for this panel.
    const bool busy = active_network || content_loading;
    if (sync_state == SyncState::Different && remote_anchor && !busy) {
        // Each card is the control for its position; nothing changes on the server here.
        if (!panel_background(508, false)) return;
        canvas.line(0, 508, 683, 508, 1, 3);
        text("两端位置不同", 36, 550, 32, 612, 1, true);
        note("点选要继续的位置。不会自动改动远端。", 612);
        const int order = further();
        outline(36, 674, 297, 250, order < 0 ? 3 : 2); outline(351, 674, 297, 250, order < 0 ? 2 : 3);
        text("本机", 56, 694, 24, 258, 3);
        title_lines(chapter_label(), 56, 738, 256, 28, true, 2);
        text(book_percent(), 56, 806, 24, 256);
        text(order < 0 ? "这台设备 · 更靠后" : "这台设备", 56, 838, 22, 256, 3);
        canvas.line(36, 872, 332, 872, 12, 2);
        center("留在这里", 36, 882, 297, 24);
        hits.push_back({36, 674, 297, 250, SyncStay});
        text("远端", 371, 694, 24, 256, 3);
        title_lines(anchor_label(*remote_anchor), 371, 738, 256, 28, true, 2);
        text(remote_book_percent(), 371, 806, 24, 256);
        const auto device = remote_device.empty() ? std::string("其他设备") : remote_device;
        text(order > 0 ? device + " · 更靠后" : device, 371, 838, 22, 256, 3);
        canvas.line(351, 872, 647, 872, 12, 2);
        center("跳到这里", 351, 882, 297, 24);
        hits.push_back({351, 674, 297, 250, SyncJump});
        // A refused or failed upload returns to this panel, which then shows its reason.
        if (sync_status.empty()) note("留在这里不会上传，下次检查仍会提示。", 948);
        else title_lines(sync_status, 36, 948, 612, 24, false, 2);
        button(SyncUpload, "上传本机位置…", 36, sync_lower, 612, 88);
        return;
    }
    if (!panel_background(508 - panel_strip)) return;
    tabs(508, Sync);
    if (txt_name(book_path)) {
        const int height = wrapped("TXT 的阅读位置只保存在本机，不支持 KOReader 接续。", 36, 650, 612, 28);
        text(local_position() + (session.needs_save() ? " · 尚未保存" : " · 已保存"), 36, 674 + height, 24, 612, 3);
        return;
    }
    if (busy) {
        text(uploading ? "正在上传本机位置" : content_loading ? "正在准备位置" : "正在检查远端位置", 36, 650, 30, 612, 1, true);
        text("网络：" + network_label(), 36, 712, 24, 612, 3);
        // The frame stays unchanged until the request ends: panel updates during TLS traffic
        // have caused display line-queue underruns.
        button(SyncCancel, uploading ? "取消上传" : "取消同步", 36, sync_lower, 612, 88);
        if (content_loading) loading_cancel = hits.back();
        note(uploading ? "已经发出的上传，取消后服务器仍可能收到。" : "取消后保留本机位置。", 1160);
        return;
    }
    const auto status = [&](const std::string& fallback) {
        title_lines(sync_status.empty() ? fallback : sync_status, 36, sync_status_top, 612, 24, false, 2);
    };
    if (!badge_configured) {
        sync_columns("—", "未设置同步");
        status("还没有设置 KOReader 同步。设置后，可以和其他设备接续阅读位置。");
        button(SyncAccount, "设置同步账户", 36, sync_upper, 612, 88, true);
        return;
    }
    if (!wifi_enabled()) {
        sync_columns("未检查", "—");
        status("Wi-Fi 已关闭。");
        button(SyncEnable, "打开 Wi-Fi 并检查", 36, sync_upper, 612, 88, true);
        return;
    }
    switch (sync_state) {
    case SyncState::NoRecord:
        sync_columns("无记录", "服务器上没有这本书");
        status("上传后，其他设备打开同一本书时可以接续。");
        button(SyncUpload, "上传本机位置…", 36, sync_lower, 612, 88, true);
        break;
    case SyncState::Same:
        sync_columns(local_position(), "与本机相同");
        status("两端位置一致，不需要操作。");
        button(SyncCheck, "再次检查", 36, sync_upper, 612, 88);
        break;
    case SyncState::Unmapped:
        sync_columns(remote_position(), remote_device);
        status("这台设备上找不到远端位置对应的段落，没有跳转。两端的书可能不是同一版本。");
        button(Back, "继续阅读", 36, sync_upper, 612, 88, true);
        button(SyncUpload, "上传本机位置…", 36, sync_lower, 612, 88);
        break;
    case SyncState::Uploaded:
        sync_columns(local_position(), "刚刚上传");
        status("已上传。其他设备下次同步会收到这个位置。");
        button(SyncCheck, "再次检查", 36, sync_upper, 612, 88);
        break;
    case SyncState::Uncertain:
        sync_columns("未确认", "—");
        status("不确定服务器是否收到了这次上传。重新检查后再决定是否上传。");
        sync_network_row();
        button(SyncCheck, "重新检查", 36, sync_upper, 612, 88, true);
        break;
    case SyncState::Unauthorized:
        sync_columns("未检查", "—");
        status("账户或密码不正确，服务器拒绝了请求。");
        sync_network_row();
        button(SyncAccount, "同步账户设置", 36, sync_upper, 612, 88, true);
        break;
    case SyncState::Failed:
        sync_columns("未检查", "—");
        status("服务器没有响应，或网络中断。");
        sync_network_row();
        button(SyncCheck, "重试", 36, sync_upper, 612, 88, true);
        break;
    case SyncState::LinkFailed:
        sync_columns("未检查", "—");
        status("没有连上 Wi-Fi。");
        sync_network_row();
        button(SyncCheck, "重试", 36, sync_upper, 612, 88, true);
        // A pinned network is never replaced silently; the user may allow it for this check.
        if (sync_link_pinned) button(SyncOnceAuto, "本次改用自动", 36, sync_lower, 612, 88);
        else button(WifiSettings, "Wi-Fi 设置", 36, sync_lower, 612, 88);
        break;
    case SyncState::ConfigChanged:
        sync_columns("未检查", "—");
        status("同步账户已更改，请重新检查。");
        button(SyncCheck, "检查远端位置", 36, sync_upper, 612, 88, true);
        break;
    default:
        sync_columns("未检查", "");
        status(sync_badge() == "待核实" ? "上次上传结果未确认，请重新检查远端。" : "先读取远端位置，再决定是否上传。");
        sync_network_row();
        button(SyncCheck, "检查远端位置", 36, sync_upper, 612, 88, true);
    }
}


void ReaderApp::Impl::check_sync(bool automatic) {
    if (txt_name(book_path)) { draw_sync(); return; }
    cancel_sync();
    sync_status.clear();
    std::string detail;
    refresh_badge_config();
    sync::HttpRequest request;
    if (!wifi_enabled() || !sync::load_config(root, sync_config, detail)) {
        // The panel shows the closed radio or the missing account with its own action.
    } else if (!sync::get_request(sync_config, document.metadata().partial_md5, request, detail)) {
        sync_state = SyncState::Failed;
        sync_status = "无法生成同步请求，请检查同步账户。";
    } else {
        auto targets = wifi_targets(detail, automatic);
        if (targets.networks.empty()) {
            sync_state = SyncState::LinkFailed; sync_link_pinned = false;
            sync_status = detail;
        } else {
            sync_link_pinned = !targets.automatic;
            sync_link_name = targets.automatic ? "" : targets.networks.front().ssid;
            sync_hold = std::make_shared<std::atomic_bool>(false);
            active_network = ++network_sequence;
            uploading = false;
            network_cancel = std::make_shared<std::atomic_bool>(false);
            network_request = NetworkOperation{active_network, std::move(request), network_cancel, sync_hold, std::move(targets)};
        }
    }
    draw_sync();
    overlay();
}


void ReaderApp::Impl::upload() {
    if (!sync_checked || !persist() || !session.saved_record()) {
        sync_status = "当前进度尚未保存，无法上传。";
        draw_sync();
        overlay();
        return;
    }
    std::string detail;
    sync::Config current;
    if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
        cancel_sync();
        sync_state = SyncState::ConfigChanged;
        draw_sync();
        overlay();
        return;
    }
    const auto record = *session.saved_record();
    if (async_content) { begin_mapping(ContentJob::Kind::MapUpload); return; }
    std::string xpointer;
    double percent = 0;
    if (!sync::map_upload_position(document, record.anchor, sync_config.dom_version,
                                   xpointer, percent, detail)) {
        upload_mapping_failed(detail);
        draw_sync();
        overlay();
        return;
    }
    send_upload(record, xpointer, percent);
}


void ReaderApp::Impl::send_upload(const ReadingRecord& record, const std::string& xpointer, double percent) {
    // The upload belongs to the session that answered the check; an ended session checks again.
    if (!network::live(sync_hold)) { check_sync(); return; }
    if (!wifi_enabled()) { cancel_sync(); draw_sync(); overlay(); return; }
    std::string detail;
    sync::HttpRequest request;
    if (!sync::put_request(sync_config, document.metadata().partial_md5, xpointer, percent,
                           record, request, detail)) {
        sync_status = "无法生成同步请求，请检查同步账户。";
        draw_sync();
        overlay();
        return;
    }
    // The link of this session is already up, so the worker uses it without choosing again.
    auto targets = wifi_targets(detail, !sync_link_pinned);
    upload_record = record;
    uploading = true;
    active_network = ++network_sequence;
    network_cancel = std::make_shared<std::atomic_bool>(false);
    network_request = NetworkOperation{active_network, std::move(request), network_cancel, sync_hold, std::move(targets)};
    sync_status.clear();
    draw_sync();
    overlay();
}


void ReaderApp::Impl::network_complete(std::uint64_t id, int status, const std::string &bytes,
                      const std::string &failure, LinkFailure link) {
    if (id != active_network || !active_network)
        return;
    if (account_check) { account_complete(status, failure, link); return; }
    if (uploading && upload_record) { uncertain_record = upload_record; uncertain_config = sync_config; }
    active_network = 0;
    if (view != View::Sync) {
        cancel_sync();
        return;
    }
    sync::Config current;
    std::string detail;
    sync_status.clear();
    if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
        cancel_sync();
        sync_state = SyncState::ConfigChanged;
    } else if (!failure.empty()) {
        sync_checked = false;
        remote_anchor.reset();
        // A failed upload may have reached the server, whatever the reported cause.
        sync_state = uploading ? SyncState::Uncertain : link != LinkFailure::None ? SyncState::LinkFailed : SyncState::Failed;
        if (sync_state == SyncState::LinkFailed)
            sync_status = sync_link_pinned && link == LinkFailure::NotNearby
                ? "指定的" + quoted_name(sync_link_name) + "不在附近，没有连接其他网络。" : failure;
    } else if (uploading) {
        remote_anchor.reset();
        if (sync::upload_confirmed(status, detail) && upload_record && session.saved_record() &&
            *session.saved_record() == *upload_record) {
            acknowledged_record = upload_record; acknowledged_config = sync_config; uncertain_record.reset();
            sync_state = SyncState::Uploaded;
        } else {
            uncertain_record = upload_record; uncertain_config = sync_config;
            sync_state = status == 401 || status == 403 ? SyncState::Unauthorized : SyncState::Uncertain;
        }
        sync_checked = false;
    } else {
        const auto remote = sync::parse_remote(status, bytes);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        const char* remote_status = remote.status == sync::RemoteStatus::Found ? "Found" :
            remote.status == sync::RemoteStatus::NoRecord ? "NoRecord" :
            remote.status == sync::RemoteStatus::Unauthorized ? "Unauthorized" : "Error";
        ESP_LOGI("reader_sync", "http_status=%d remote_status=%s", status, remote_status);
#endif
        if (remote.status == sync::RemoteStatus::NoRecord) {
            uncertain_record.reset(); acknowledged_record.reset();
            sync_checked = true; sync_state = SyncState::NoRecord;
        } else if (remote.status == sync::RemoteStatus::Found) {
            uncertain_record.reset(); acknowledged_record.reset();
            Anchor candidate;
            sync::PartPosition place;
            sync_checked = true;
            remote_percent = remote.percentage;  // parse_remote() accepts only [0, 1].
            remote_device = remote.device;
            if (async_content) { begin_mapping(ContentJob::Kind::MapDownload, remote.progress); return; }
            sync::from_xpointer(document, remote.progress, sync_config.dom_version, candidate, detail, {}, &place);
            accept_remote_mapping(candidate, place, detail);
        } else {
            sync_checked = false;
            sync_state = remote.status == sync::RemoteStatus::Unauthorized ? SyncState::Unauthorized : SyncState::Failed;
        }
    }
    if (!sync_checked || sync_state == SyncState::Same) network::release(sync_hold);
    uploading = false;
    upload_record.reset();
    draw_sync();
    overlay();
}


} // namespace pico_page
