#include "app_impl.h"
#include "clock_panel.h"

namespace pico_page {

void ReaderApp::Impl::draw_sync_settings() {
    if (!bookmark_io_ready()) return;
    view = View::SyncSettings;
    hits.clear(); canvas.clear();
    if (account_page == AccountPage::Remove) {
        header(tr("移除同步账户"), 0);
        text(tr("移除同步账户？"), 36, 160, 34, 612, 1, true);
        wrapped(tr("设备会删除 PicoPage/sync.toml。阅读位置仍保存在 TF 卡，服务器上的记录不受影响。"), 36, 244, 612, 28);
        button(Back, tr("取消"), 24, 1080, 306, 96, true);
        button(AccountRemoveConfirm, tr("移除"), 354, 1080, 306, 96);
        return;
    }
    if (account_page == AccountPage::Verifying) {
        header(tr("KOReader 同步"), 0);
        text(tr("正在验证账户"), 36, 160, 34, 612, 1, true);
        text(trf("网络：{network}", {{"network", network_label()}}), 36, 236, 24, 612, 3);
        button(AccountCancel, tr("取消"), 24, 1000, 636, 96);
        note(account.had_saved_config ? tr("取消后保留原来的账户。") : tr("取消后不保存账户。"), 1124);
        return;
    }
    header(tr("KOReader 同步"), Back, tr("设置"));
    sync::Config next;
    const bool complete = account_config(next);
    note(!account.had_saved_config || account.changed ? tr("账户 · 未保存")
         : account.check == AccountDraft::Check::Rejected ? tr("账户 · 验证未通过")
         : account.check == AccountDraft::Check::Accepted ? tr("账户 · 已验证") : tr("账户 · 已保存"), 130);
    value_row(AccountEndpoint, tr("服务地址"), "", server_name(next), 170, 96);
    value_row(AccountUser, tr("账户"), "", account.username.empty() ? tr("未填写") : account.username, 266, 96);
    value_row(AccountPassword, tr("密码"), "", !account.password.empty() ? tr("已填写") : !next.userkey.empty() ? tr("已保存") : tr("未填写"), 362, 96);
    wrapped(tr("验证时需要联网。也可以在电脑上编辑 PicoPage/sync.toml。"), 36, 482, 612, 22, true, 3);
    const auto verify = account.had_saved_config && !account.changed ? tr("重新验证") : tr("验证并保存账户");
    if (complete) button(AccountVerify, verify, 24, 580, 636, 96, account.changed || !account.had_saved_config);
    else disabled_button(verify, 24, 580, 636, 96);
    if (account.had_saved_config) button(AccountRemove, tr("移除同步账户"), 24, 700, 636, 96);
    if (has_book() && account.had_saved_config && !account.changed && !txt_name(book_path))
        value_row(Sync, trf("检查《{title}》的同步", {{"title", document.metadata().title}}), "", "", 828, 96);
    else if (has_book() && txt_name(book_path)) note(tr("TXT 只保存本机进度，不支持 KOReader 接续。"), 860);
    wrapped(account_notice, 36, 1040, 612, 24);
}

void ReaderApp::Impl::verify_account() {
    sync::Config next;
    sync::HttpRequest request;
    std::string problem;
    if (!account_config(next) || !sync::build_auth_request(next, request, problem)) {
        account_notice = tr("服务地址必须以 https:// 开头，账户和密码不能为空。");
    } else if (!wifi_enabled()) account_notice = tr("Wi-Fi 已关闭。请先在 Wi-Fi 设置中打开。");
    else if (wifi_worker || portal_live || active_network) account_notice = tr("请先结束当前任务。");
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

void ReaderApp::Impl::draw_sync_comparison() {
    if (!panel_background(508, false)) return;
    canvas.line(0, 508, 683, 508, 1, 3);
    text(tr("两端位置不同"), 36, 550, 32, 612, 1, true);
    note(tr("点选要继续的位置。不会自动改动远端。"), 612);
    const int order = further();
    outline(36, 674, 297, 250, order < 0 ? 3 : 2); outline(351, 674, 297, 250, order < 0 ? 2 : 3);
    text(tr("本机"), 56, 694, 24, 258, 3);
    title_lines(body, chapter_label(), 56, 738, 256, 28, true, 2);
    text(book_percent(), 56, 806, 24, 256);
    text(order < 0 ? tr("这台设备 · 更靠后") : tr("这台设备"), 56, 838, 22, 256, 3);
    canvas.line(36, 872, 332, 872, 12, 2);
    center(tr("留在这里"), 36, 882, 297, 24);
    hits.push_back({36, 674, 297, 250, SyncStay});
    text(tr("远端"), 371, 694, 24, 256, 3);
    title_lines(body, anchor_label(*remote_anchor), 371, 738, 256, 28, true, 2);
    text(remote_book_percent(), 371, 806, 24, 256);
    const auto device = remote_device.empty() ? std::string(tr("其他设备")) : remote_device;
    text(order > 0 ? device + tr(" · 更靠后") : device, 371, 838, 22, 256, 3);
    canvas.line(351, 872, 647, 872, 12, 2);
    center(tr("跳到这里"), 351, 882, 297, 24);
    hits.push_back({351, 674, 297, 250, SyncJump});
    if (sync_status.empty()) title_lines(tr("留在这里不会上传，下次检查仍会提示。"), 36, 948, 612, 24, false, 2, 3);
    else title_lines(sync_status, 36, 948, 612, 24, false, 2);
    button(SyncUpload, tr("上传本机位置…"), 36, 1052, 612, 88);
}

void ReaderApp::Impl::sync_information_card(const std::string& label, const std::string& value,
                                            const std::string& detail, int x, int width,
                                            graphics::Font& value_font, int value_size) {
    constexpr int top = 816, height = 136, padding = 20;
    outline(x, top, width, height);
    text(label, x + padding, top_for_middle(ui, 24, top + 26), 24, width - 2 * padding, 3);
    text(value_font, value, x + padding, top_for_middle(value_font, value_size, top + 68, true),
         value_size, width - 2 * padding, 1, true);
    text(detail, x + padding, top_for_middle(ui, 22, top + 111), 22, width - 2 * padding, 3);
}

void ReaderApp::Impl::sync_hint(const std::string& hint, std::uint8_t gray) {
    constexpr int width = 612, size = 24, middle = 1092;
    const int center_top = top_for_middle(ui, size, middle);
    if (text_width(hint, size) <= width) text(hint, 36, center_top, size, width, gray);
    else title_lines(hint, 36, center_top - 16, width, size, false, 2, gray);
}

void ReaderApp::Impl::sync_actions(SyncAction left, SyncAction right) {
    const auto draw = [&](SyncAction action, int x) {
        if (action.id)
            button(action.id, action.label, x, typography_actions_top, panel_card_width, typography_action_height, action.primary);
        else disabled_button(action.label, x, typography_actions_top, panel_card_width, typography_action_height);
    };
    draw(left, panel_card_left);
    draw(right, panel_card_right);
}

void ReaderApp::Impl::draw_sync() {
    if (!bookmark_io_ready()) return;
    view = View::Sync;
    if (!has_book()) {
        hits.clear(); canvas.clear(); header(tr("同步"), Back, tr("书库"));
        note(tr("请先打开一本 EPUB。"), 246);
        return;
    }
    const bool progress = sync_subtab == SyncSubtab::Progress;
    const bool busy = active_network || content_loading || wifi_worker || network_worker_id;
    if (progress && sync_state == SyncState::Different && remote_anchor && !busy) {
        draw_sync_comparison();
        return;
    }

    constexpr int tabs_top = 612, strip_top = tabs_top - panel_strip;
    constexpr int choices_top = 728, network_top = 964;
    constexpr int row_width = panel_card_right + panel_card_width - panel_card_left;
    if (!panel_background(strip_top)) return;
    hits.push_back({0, 104, 684, strip_top - 104, Back});
    tabs(tabs_top, Sync);

    const auto now = std::time(nullptr);
    const auto summary = !progress
        ? clock_running() ? network::live(wifi_hold) ? tr("设备时钟 · 正在校准") : tr("设备时钟 · 结束中")
            : clock_time::valid(now) ? tr("设备时钟 · UTC+8") : tr("设备时钟 · 尚未校准")
        : txt_name(book_path) ? tr("TXT · 仅本机保存")
        : !badge_configured ? tr("阅读进度 · 未设置账户")
        : uploading ? tr("阅读进度 · 正在上传")
        : content_loading ? tr("阅读进度 · 正在准备")
        : active_network ? tr("阅读进度 · 正在检查")
        : sync_state == SyncState::NoRecord ? tr("阅读进度 · 远端无记录")
        : sync_state == SyncState::Same ? tr("阅读进度 · 两端一致")
        : sync_state == SyncState::Uploaded ? tr("阅读进度 · 已上传")
        : tr("阅读进度 · 未检查");
    text(summary, 36, top_for_middle(ui, 24, strip_top + panel_strip / 2), 24, 470, 3);

    panel_choice_card(busy ? 0 : SyncSubProgress, tr("阅读进度"), panel_card_left,
                      choices_top, progress, progress);
    panel_choice_card(busy ? 0 : SyncSubClock, tr("校准时间"), panel_card_right,
                      choices_top, !progress, !progress);

    const bool enabled = wifi_enabled();
    std::string network_problem;
    const auto targets = wifi_targets(network_problem);
    const bool saved_network = !targets.networks.empty();
    const auto name = network_label(saved_network && !targets.automatic ? targets.networks.front().ssid : "");
    const auto network_value = !enabled ? tr("Wi-Fi 已关闭") : saved_network ? name : tr("未保存网络");
    const int network_action = busy ? 0 : enabled && saved_network ? WifiModeOpen : WifiSettings;
    panel_navigation_card(network_action, tr("网络"), network_value, panel_card_left, network_top, row_width);

    if (progress) draw_sync_progress(enabled, saved_network, network_problem);
    else draw_sync_clock(now, enabled, saved_network, network_problem, name);
}

void ReaderApp::Impl::draw_sync_clock(std::time_t now, bool enabled, bool saved_network, const std::string& network_problem,
                                      const std::string& network_name) {
    const bool clock_valid = clock_time::valid(now);
    const auto current = clock_valid ? reading_footer::clock(now, clock_time::utc8_offset_seconds / 60) : tr("尚未校准");
    const auto detail = clock_valid ? "UTC+8 · " + clock_panel::local_date(now) : tr("UTC+8 · 时钟未就绪");
    const auto last = clock_panel::last_calibration(now, last_clock_sync_at, saved_network, settings.language);
    sync_information_card(tr("当前时间"), current, detail, panel_card_left, panel_card_width, ui, clock_valid ? 34 : 28);
    sync_information_card(tr("上次校准"), last.value, last.detail, panel_card_right, panel_card_width, ui);

    const SyncAction settings_action{WifiSettings, tr("Wi-Fi 设置")};
    const SyncAction settings_disabled{0, settings_action.label};
    if (clock_running()) {
        if (network::live(wifi_hold)) {
            sync_hint(trf("正在经「{network}」获取网络时间…", {{"network", network_name}}), 1);
            sync_actions(settings_disabled, {WifiClockCancel, tr("取消校准")});
        } else {
            sync_hint(tr("正在结束校准，等待网络释放…"), 1);
            sync_actions(settings_disabled, {0, tr("结束中")});
        }
    } else if (wifi_worker || active_network || network_worker_id || content_loading) {
        sync_hint(tr("请先结束当前任务。"), 1);
        sync_actions(settings_disabled, {0, tr("立即校准")});
    } else if (!saved_network) {
        sync_hint(clock_notice.empty() ? network_problem : clock_notice, 1);
        sync_actions({WifiSettings, settings_action.label, true}, {0, tr("暂无可用网络")});
    } else if (!enabled) {
        sync_hint(clock_notice.empty() ? tr("Wi-Fi 已关闭。可打开无线并校准。") : clock_notice, 1);
        sync_actions(settings_action, {SyncClockEnable, tr("打开 Wi-Fi 并校准"), true});
    } else {
        sync_hint(clock_notice.empty() ? tr("离线时钟会逐渐偏移，临时联网校准后自动断开。") : clock_notice,
                  clock_notice.empty() ? 3 : 1);
        sync_actions(settings_action, {WifiClock, clock_feedback == ClockFeedback::Failed ? tr("重试") : tr("立即校准"), true});
    }
}

void ReaderApp::Impl::draw_sync_progress(bool enabled, bool saved_network, const std::string& network_problem) {
    const auto status = [&](const std::string& fallback) {
        sync_hint(sync_status.empty() ? fallback : sync_status, sync_status.empty() ? 3 : 1);
    };
    const SyncAction account_action{SyncAccount, tr("同步账户")};
    const SyncAction wifi_settings{WifiSettings, tr("Wi-Fi 设置")};
    const auto saved = session.needs_save() ? tr("尚未保存") : tr("已保存");
    if (txt_name(book_path) || !badge_configured) {
        const bool txt = txt_name(book_path);
        constexpr int width = panel_card_right + panel_card_width - panel_card_left;
        sync_information_card(tr("本机") + std::string(" · ") + saved, local_position(),
                              txt ? tr("TXT 仅保存本机阅读位置，不支持 KOReader 同步。")
                                  : tr("尚未设置 KOReader 同步账户，设置后可跨设备接续。"),
                              panel_card_left, width, body);
        if (wifi_worker || network_worker_id) {
            status(tr("请先结束当前任务。"));
            sync_actions({0, txt ? tr("校准时间") : tr("完成")},
                         {0, txt ? tr("完成") : tr("设置同步账户")});
        } else if (txt) sync_actions({SyncSubClock, tr("校准时间")}, {Back, tr("完成")});
        else sync_actions({Back, tr("完成")}, {SyncAccount, tr("设置同步账户"), true});
        return;
    }

    sync_information_card(tr("本机"), local_position(), tr("这台设备") + std::string(" · ") + saved,
                          panel_card_left, panel_card_width, body);
    const auto remote = [&](const std::string& value, const std::string& detail, bool position = false) {
        sync_information_card(tr("远端"), value, detail, panel_card_right, panel_card_width, position ? body : ui);
    };
    if (active_network || content_loading) {
        remote(uploading ? tr("上传中…") : content_loading ? tr("准备中…") : tr("检查中…"), "");
        sync_hint(uploading ? tr("已发出的上传在取消后仍可能生效。") : tr("取消后保留本机位置。"), 1);
        sync_actions({0, account_action.label}, {SyncCancel, uploading ? tr("取消上传") : tr("取消同步")});
        if (content_loading) loading_cancel = hits.back();
        return;
    }
    if (wifi_worker || network_worker_id) {
        remote(tr("未检查"), "");
        status(tr("请先结束当前任务。"));
        sync_actions({0, account_action.label}, {0, tr("检查远端位置")});
        return;
    }
    if (!saved_network) {
        remote(tr("未检查"), "");
        status(network_problem);
        sync_actions({WifiSettings, wifi_settings.label, true}, {0, tr("暂无可用网络")});
        return;
    }
    if (!enabled) {
        remote(tr("未检查"), "");
        status(tr("Wi-Fi 已关闭。可打开无线并检查远端位置。"));
        sync_actions(account_action, {SyncEnable, tr("打开 Wi-Fi 并检查"), true});
        return;
    }
    switch (sync_state) {
    case SyncState::NoRecord:
        remote(tr("无记录"), tr("没有阅读位置"));
        status(tr("远端无记录。可上传本机位置供其他设备接续。"));
        sync_actions({SyncUpload, tr("上传本机位置…"), true}, {SyncCheck, tr("再次检查")});
        break;
    case SyncState::Same:
        remote(local_position(), tr("与本机相同"), true);
        status(tr("两端位置一致，不需要操作。"));
        sync_actions(account_action, {SyncCheck, tr("再次检查")});
        break;
    case SyncState::Unmapped:
        remote(remote_position(), remote_device, remote_anchor.has_value());
        status(tr("找不到远端位置对应的段落，未跳转。两端的书可能是不同版本。"));
        sync_actions({SyncUpload, tr("上传本机位置…")}, {Back, tr("继续阅读"), true});
        break;
    case SyncState::Uploaded:
        remote(local_position(), tr("刚刚上传"), true);
        status(tr("已上传。其他设备下次同步会收到这个位置。"));
        sync_actions(account_action, {SyncCheck, tr("再次检查")});
        break;
    case SyncState::Uncertain:
        remote(tr("未确认"), "");
        status(tr("上传结果未确认。请重新检查远端。"));
        sync_actions(account_action, {SyncCheck, tr("重新检查"), true});
        break;
    case SyncState::Unauthorized:
        remote(tr("未检查"), "");
        status(tr("账户或密码不正确，服务器拒绝了请求。"));
        sync_actions({Back, tr("完成")}, {SyncAccount, tr("同步账户设置"), true});
        break;
    case SyncState::Failed:
        remote(tr("未检查"), "");
        status(tr("没有取得远端位置。"));
        sync_actions(account_action, {SyncCheck, tr("重试"), true});
        break;
    case SyncState::LinkFailed:
        remote(tr("未检查"), "");
        status(tr("没有连上 Wi-Fi。"));
        sync_actions(sync_link_pinned ? SyncAction{SyncOnceAuto, tr("本次改用自动")} : wifi_settings,
                     {SyncCheck, tr("重试"), true});
        break;
    case SyncState::ConfigChanged:
        remote(tr("未检查"), "");
        status(tr("同步账户已更改，请重新检查。"));
        sync_actions(account_action, {SyncCheck, tr("检查远端位置"), true});
        break;
    default:
        remote(tr("未检查"), "");
        status(sync_badge() == tr("待核实") ? tr("上次上传结果未确认，请重新检查远端。")
                                          : tr("先读取远端位置，再决定是否上传。"));
        sync_actions(account_action, {SyncCheck, tr("检查远端位置"), true});
    }
}

void ReaderApp::Impl::start_clock_sync() {
    std::string problem;
    if (wifi_worker || active_network || network_worker_id || content_loading || portal_live)
        problem = tr("请先结束当前任务。");
    else if (!wifi_enabled()) problem = tr("请先打开 Wi-Fi。");
    else {
        auto targets = wifi_targets(problem);
        if (!targets.networks.empty()) {
            network::release(sync_hold);
            start_wifi(WifiOperation::Kind::Clock, std::move(targets), wifi_hold);
        }
    }
    clock_feedback = problem.empty() ? ClockFeedback::Idle : ClockFeedback::Failed;
    clock_notice = std::move(problem);
    draw_sync(); overlay();
}

void ReaderApp::Impl::check_sync(bool automatic) {
    if ((view != View::Sync && view != View::Confirmation) || sync_subtab != SyncSubtab::Progress || wifi_worker ||
        active_network || network_worker_id || content_loading || portal_live || txt_name(book_path)) return;
    cancel_sync();
    sync_status.clear();
    std::string detail;
    refresh_badge_config();
    sync::HttpRequest request;
    if (!wifi_enabled() || !sync::load_config(root, sync_config, detail)) {
    } else if (!sync::build_get_request(sync_config, document.metadata().partial_md5, request, detail)) {
        sync_state = SyncState::Failed;
        sync_status = tr("无法生成同步请求，请检查同步账户。");
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
        sync_status = tr("当前进度尚未保存，无法上传。");
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
    double progress_fraction = 0;
    if (!sync::map_upload_position(document, record.anchor, sync_config.dom_version,
                                   xpointer, progress_fraction, detail)) {
        upload_mapping_failed(detail);
        draw_sync();
        overlay();
        return;
    }
    send_upload(record, xpointer, progress_fraction);
}

void ReaderApp::Impl::send_upload(const ReadingRecord& record, const std::string& xpointer, double progress_fraction) {
    if (!network::live(sync_hold)) { check_sync(); return; }
    if (!wifi_enabled()) { cancel_sync(); draw_sync(); overlay(); return; }
    std::string detail;
    sync::HttpRequest request;
    if (!sync::build_put_request(sync_config, document.metadata().partial_md5, xpointer, progress_fraction,
                           record, request, detail)) {
        sync_status = tr("无法生成同步请求，请检查同步账户。");
        draw_sync();
        overlay();
        return;
    }
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
        sync_state = uploading ? SyncState::Uncertain : link != LinkFailure::None ? SyncState::LinkFailed : SyncState::Failed;
        if (sync_state == SyncState::LinkFailed)
            sync_status = sync_link_pinned && link == LinkFailure::NotNearby
                ? trf("指定的「{network}」不在附近，没有连接其他网络。", {{"network", sync_link_name}}) : failure;
        else if (sync_state == SyncState::Failed) sync_status = tr("没有连上同步服务器。");
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
            remote_fraction = remote.progress_fraction;
            remote_device = remote.device;
            if (async_content) { begin_mapping(ContentJob::Kind::MapDownload, remote.progress); return; }
            sync::from_xpointer(document, remote.progress, sync_config.dom_version, candidate, detail, {}, &place);
            accept_remote_mapping(candidate, place, detail);
        } else {
            sync_checked = false;
            sync_state = remote.status == sync::RemoteStatus::Unauthorized ? SyncState::Unauthorized : SyncState::Failed;
            if (sync_state == SyncState::Failed)
                sync_status = status != 200 ? trf("同步服务返回 HTTP {status}，没有取得远端位置。", {{"status", std::to_string(status)}})
                                            : tr("同步服务返回的内容无法识别，没有取得远端位置。");
        }
    }
    if (!sync_checked || sync_state == SyncState::Same) network::release(sync_hold);
    uploading = false;
    upload_record.reset();
    draw_sync();
    overlay();
}

}
