#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::poll_save() {
    if (!save_job) return;
    if (!save_job->started()) {
        if (counting_job || cover_job || (bookmark_job && bookmark_job->started()) ||
            (foreground_job && foreground_job->started() && !foreground_job->done())) return;
        if (save_job->start()) return;
        save_job->result.progress.status = StoreStatus::IoError;
    } else if (!save_job->done()) return;
    auto job = std::move(save_job);
    const auto request = std::exchange(saving_request, std::nullopt);
    const bool okay = job->result.progress.status == StoreStatus::Ok &&
        job->result.progress.record && *job->result.progress.record == *job->request.record;
    if (request) session.saved(*request, okay);
    if (pending_position) {
        const auto &saved = *job->request.record;
        if (okay && saved.book_id == pending_position->book_id && saved.anchor == pending_position->anchor)
            store.save_cache(pending_position->name, pending_position->bytes);
        pending_position.reset();
    }
    save_failed = !okay;
    save_error_pending = !okay;
    if (!okay) message = kSaveFailed;
    if (job->result.last_book_saved) last_recorded = job->request.relative_path;
    else if (okay && job->request.save_last_book) {
        recent_save_failed = true; message = "最近阅读记录未保存";
    }
}



void ReaderApp::Impl::apply_typography() {
    if (!bookmark_io_ready()) return;
    std::string detail;
    if (font_fallback || weight_unavailable) {
        error("所选字体或字重不可用，排版未保存");
        return;
    }
    const auto result = reset_overrides && book_scope
                            ? store.clear_book_settings(edit_snapshot, detail)
                            : store.apply_settings(edit_snapshot, draft, book_scope, detail);
    if (result != StoreStatus::Ok) {
        error("排版未保存。" + reason(detail) + "。");
        return;
    }
    const auto effective = store.load_settings(book_path).settings;
    const bool layout_changed = effective.font != settings.font ||
        effective.font_weight != settings.font_weight ||
        effective.font_size_px != settings.font_size_px ||
        effective.line_height != settings.line_height ||
        effective.margin_x_px != settings.margin_x_px ||
        effective.paragraph_gap_lines != settings.paragraph_gap_lines ||
        effective.indent_chars != settings.indent_chars;
    settings = effective;
    if (!book_scope) {
        load_library_font(store.load_settings().settings.font);
        message = layout_changed ? "阅读默认已保存，当前书已应用" : "阅读默认已保存，本书排版未变";
        if (!layout_changed) {
            load_fonts();
            draw_tools();
            overlay();
            return;
        }
    }
    load_fonts();
    clear_page_history();
    reset_pages();
    abandon_content();
    session.change_layout();
    show_page(offset);
}
void ReaderApp::Impl::apply_refresh(int id) {
    auto values = edit_snapshot.config.settings;
    values.refresh_preset = id == RefreshClear ? "clear" : id == RefreshFast ? "fast" : "balanced";
    save_refresh(values);
}
void ReaderApp::Impl::save_refresh(const ReaderSettings &values) {
    if (!bookmark_io_ready()) return;
    std::string detail;
    if (store.apply_settings(edit_snapshot, values, !refresh_from_settings, detail) != StoreStatus::Ok) {
        error("刷新设置未保存。" + reason(detail) + "。");
        return;
    }
    // Refresh edits do not apply external typography changes to the visible page.
    const auto effective = store.load_settings(book_path).settings;
    settings.refresh_preset = effective.refresh_preset;
    settings.balanced_full_refresh_pages = effective.balanced_full_refresh_pages;
    settings.fast_full_refresh_pages = effective.fast_full_refresh_pages;
    edit_snapshot = store.settings_snapshot(refresh_from_settings ? "" : book_path);
    draw_refresh();
    overlay();
}

ReaderApp::ReaderApp(std::string root, std::string writer, bool controlled_storage, bool async_content)
    : impl_(std::make_unique<Impl>(std::move(root), std::move(writer))) { impl_->controlled_storage = controlled_storage; impl_->async_content = async_content; }
void ReaderApp::invalidate_identity_cache() {
    impl_->library_font_reload = true;
    impl_->drain_save(); impl_->abandon_content(); impl_->foreground_job.reset(); impl_->stop_covers();
    impl_->clear_prefetch(); impl_->stop_counting_job(); impl_->identities.clear();
    impl_->font_identity.clear(); impl_->font_scan.reset(); impl_->effective_font.clear();
    impl_->reset_dictionary_contact();
    impl_->dictionary_session.reset(); impl_->dictionary_ready.clear(); impl_->dictionary_definition.reset();
    impl_->dictionary_pages.clear(); impl_->dictionary_page = {};
}
ReaderApp::~ReaderApp() { network::release(impl_->manual_hold); impl_->cancel_sync(); impl_->drain_save(); }
bool ReaderApp::initialize() {
#ifdef ESP_PLATFORM
    if (impl_->async_content && ContentJob::prepare_worker() != ContentJob::StartFailure::None)
        ESP_LOGW("reader_app", "Content worker reservation failed; content requests will retry");
#endif
    return impl_->initialize();
}
bool ReaderApp::open_book(const std::string &path) {
    if (impl_->portal_live || impl_->active_network || impl_->update_waiting()) return false;
    impl_->reset_dictionary_contact();
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    return !impl_->usb_prepared && impl_->open(path);
}
void ReaderApp::set_battery(std::optional<int> percent, bool charging, bool allow_refresh) {
    impl_->battery_percent = percent && *percent >= 0 && *percent <= 100 ? percent : std::nullopt;
    impl_->battery_charging = charging;
    if (allow_refresh && impl_->footer_charging != charging && impl_->view == View::Reading &&
        impl_->footer_device == reading_footer::Device::Battery &&
        !impl_->display.busy() && !impl_->content_loading && !impl_->image_viewing &&
        !impl_->sleep_wait_footer && !impl_->sleep_wait_restore_pending && !impl_->sleep_destination_captured) {
        impl_->footer();
        impl_->display.present({{"ui", 0, 0, ++impl_->ui_sequence}, {}, FramePurpose::Overlay,
            RefreshIntent::Content, RefreshRegion{0, footer_top, 684, 1216 - footer_top}});
    }
}
void ReaderApp::set_firmware_version(const std::string& version) { impl_->running_version = version; }
std::optional<UpdateOperation> ReaderApp::take_update_request() {
    if (impl_->display.busy() || impl_->update_worker || !impl_->update_request) return std::nullopt;
    impl_->update_worker = impl_->update_request->id;
    return std::exchange(impl_->update_request, std::nullopt);
}
void ReaderApp::update_progress(std::uint64_t id, std::uint32_t bytes) { impl_->update_progress(id, bytes); }
void ReaderApp::complete_update(std::uint64_t id, UpdateFailure failure, int status, const std::string& body,
                                const std::string& detail, LinkFailure link, const std::string& ssid) {
    impl_->update_complete(id, failure, status, body, detail, link, ssid);
}
bool ReaderApp::update_busy() const { return impl_->update_waiting(); }
void ReaderApp::record_install() {
    impl_->update_state.pending_from = impl_->running_version;
    impl_->update_state.pending_to = impl_->update_to;
    impl_->save_update_state();
}
void ReaderApp::input(const AppInput &event) {
    impl_->input(event);
}
bool ReaderApp::can_long_press_at(int x, int y) const {
    if (impl_->image_at(x, y)) return true;
    if (impl_->view == View::Reading && !impl_->image_viewing && graphics::text_hit(impl_->page, x, y)) return true;
    return impl_->view == View::Library && std::any_of(impl_->hits.begin(), impl_->hits.end(), [&](const auto& hit) {
        return hit.contains(x, y) && (hit.id == Tools || (hit.id >= EntryBase && hit.id < TocBase &&
               !impl_->entries[static_cast<std::size_t>(hit.id - EntryBase)].directory));
    });
}
bool ReaderApp::can_drag_selection_at(int x, int y) const {
    return impl_->view == View::Dictionary && std::any_of(impl_->hits.begin(), impl_->hits.end(), [&](const auto& hit) {
        return (hit.id == DictionaryStart || hit.id == DictionaryEnd) && hit.contains(x, y);
    });
}
const graphics::Canvas &ReaderApp::canvas() const {
    return impl_->canvas;
}
const std::optional<Frame> &ReaderApp::pending_frame() const {
    return impl_->pending;
}
bool ReaderApp::prepare_frame() {
    if (!impl_->pending) return true;
    const auto paused = [](const std::unique_ptr<ContentJob>& job) {
        return !job || !job->started() || job->done() ||
            (job->request.kind != ContentJob::Kind::Pagination &&
             job->request.kind != ContentJob::Kind::Chapter && !ContentJob::dictionary_kind(job->request.kind)) || job->pause();
    };
    const bool foreground = paused(impl_->foreground_job);
    const bool background = paused(impl_->counting_job);
    return foreground && background;
}
void ReaderApp::complete_frame(bool success) {
    if (!impl_->pending)
        return;
    const auto frame = *impl_->pending;
    const auto result = impl_->display.finish(frame.token, success);
    impl_->pending.reset();
    if (impl_->foreground_job) impl_->foreground_job->resume();
    if (impl_->counting_job) impl_->counting_job->resume();
    if (impl_->view == View::Sleeping && !impl_->custom_rotation_name.empty()) {
        // Only a presented image advances the rotation.
        if (success) impl_->custom_rotation_shown = impl_->custom_rotation_name;
        impl_->custom_rotation_name.clear();
    }
    if (success) {
        if (impl_->view == View::Reading && !impl_->image_viewing && impl_->body_images_pending && !impl_->inline_image_pending) {
            const auto delay = frame.purpose == FramePurpose::Body && !impl_->session.visible_record()
                ? std::chrono::milliseconds(0) : inline_image_delay;
            impl_->inline_image_after = std::chrono::steady_clock::now() + delay;
        }
        if (frame.refresh == RefreshIntent::FullClean)
            impl_->body_updates = 0;
        else if (frame.purpose == FramePurpose::Body)
            ++impl_->body_updates;
    }
    if (result && impl_->session.presented(*result)) {
        if (frame.purpose == FramePurpose::Body) impl_->remember_page();
        impl_->confirmed_link_return = impl_->link_return;
        impl_->confirmed_sync_return = impl_->sync_return;
        impl_->save_failed = false;
        if (!impl_->async_content && !impl_->recent_save_failed && impl_->last_recorded != impl_->book_path) {
            std::string error;
            if (impl_->store.save_last_book(impl_->book_path, error) != StoreStatus::Ok) {
                impl_->recent_save_failed = true;
                impl_->message = "最近阅读记录未保存";
            } else
                impl_->last_recorded = impl_->book_path;
        }
    }
    if (success && impl_->view == View::Tools && std::exchange(impl_->tools_save_pending, false)) {
        impl_->schedule_save();
    }
    if (auto work = std::exchange(impl_->deferred, nullptr); work && success)
        work();
    // A final-frame failure must not restart application work during shutdown.
    if (impl_->view == View::PowerOff) return;
    if (success && impl_->sleep_wait_restore_pending) {
        if (impl_->sleep_wait_footer) impl_->restore_sleep_wait();
        else impl_->sleep_wait_restore_pending = false;
    }
    if (!success) {
        impl_->abandon_content();
        if (impl_->usb_prepared) {
            impl_->message = "屏幕更新失败，请退出并重启。";
            impl_->draw_usb(); impl_->overlay(true);
        } else if (impl_->view == View::PowerMenu && impl_->power_return_canvas) {
            impl_->close_power_menu();
        } else {
            const auto origin = impl_->origin_for(impl_->view);
            const bool return_help = (impl_->view == View::Onboarding && impl_->onboarding_from_help) ||
                (impl_->view == View::Error && impl_->error_onboarding_help);
            const auto destination = return_help ? "回到使用说明。" : origin == Impl::Origin::Reading ? "恢复已确认位置。" :
                                     origin == Impl::Origin::Settings ? "回到设置首页。" : "回到书库。";
            impl_->error(std::string(frame.region ? "屏幕局部更新失败。返回将" : "屏幕更新失败。返回将") + destination);
        }
    }
}
View ReaderApp::view() const {
    return impl_->view;
}
const std::string &ReaderApp::status() const {
    return impl_->message;
}
const std::optional<ReadingRecord> &ReaderApp::saved_record() const {
    return impl_->session.saved_record();
}
bool ReaderApp::fast_page_updates() const {
    return impl_->settings.refresh_preset == "fast";
}
PageEffect ReaderApp::page_effect() const { return impl_->page_effect; }
void ReaderApp::set_page_effect(PageEffect effect) {
    impl_->page_effect = effect;
    if (impl_->view == View::PageEffects) {
        impl_->draw_page_effects();
        if (!impl_->display.busy()) impl_->overlay();
    }
}
int ReaderApp::auto_sleep_minutes() const {
    return impl_->settings.auto_sleep_minutes;
}
UsbTransferAction ReaderApp::take_usb_transfer_action() {
    if (impl_->usb_action == UsbTransferAction::Start) {
        if (impl_->usb_request_view && impl_->view != *impl_->usb_request_view) impl_->usb_action = UsbTransferAction::None;
        else if (impl_->cancel_custom_preparation()) return UsbTransferAction::None;
    }
    impl_->usb_request_view.reset();
    return std::exchange(impl_->usb_action, UsbTransferAction::None);
}
bool ReaderApp::prepare_usb_transfer() {
    if (impl_->foreground_job && ContentJob::dictionary_kind(impl_->foreground_job->request.kind) &&
        impl_->foreground_job->started() && !impl_->foreground_job->done()) {
        impl_->foreground_job->cancel(); return false;
    }
    if (impl_->cancel_custom_preparation()) return false;
    invalidate_identity_cache();
    if (impl_->network_worker_id || impl_->portal_live || impl_->usb_prepared || impl_->display.busy() || impl_->active_network ||
        !impl_->card_available || !impl_->persist()) return false;
    impl_->cancel_sync();
    impl_->save_pages();
    if (!impl_->ui.open_fallback()) return false;
    impl_->library_font.close();
    impl_->library_uses_body = false; impl_->library_font_path.clear();
    impl_->document.close();
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    impl_->body.open_fallback();
    release(impl_->chapter);
    impl_->page = {};
    impl_->links.clear();
    impl_->entries.clear();
    impl_->clear_page_history();
    impl_->covers.clear();
    impl_->card_available = false;
    impl_->usb_prepared = true;
    return true;
}
void ReaderApp::show_usb_transfer(bool active, const std::string& error) {
    impl_->message = error;
    impl_->usb_active = active;
    impl_->usb_confirm = false;
    if (impl_->usb_prepared) impl_->draw_usb();
    else impl_->draw_import();
    impl_->overlay();
}
PortalAction ReaderApp::take_portal_action() {
    if (impl_->portal_action == PortalAction::Upload || impl_->portal_action == PortalAction::UploadStation) {
        if (impl_->view != impl_->portal_request_view) {
            impl_->portal_action = PortalAction::None;
            impl_->portal_requested = false;
        } else if (impl_->cancel_custom_preparation()) return PortalAction::None;
    }
    return std::exchange(impl_->portal_action, PortalAction::None);
}
std::uint64_t ReaderApp::portal_generation() const { return impl_->portal_generation; }
bool ReaderApp::wifi_enabled() const { return impl_->card_available && impl_->wifi_enabled(); }
void ReaderApp::stop_wifi(bool for_sleep) {
    if (!for_sleep) impl_->sleep_wifi_target.reset();
    else if (network::live(impl_->manual_hold) && impl_->manual_target)
        impl_->sleep_wifi_target = impl_->manual_target;
    network::release(impl_->manual_hold);
    network::release(impl_->sync_hold);
    network::release(impl_->wifi_hold);
    if (impl_->wifi_request) { impl_->wifi_request.reset(); impl_->wifi_worker = 0; }
    if (impl_->update_waiting()) {
        impl_->update_cancel();
        if (impl_->view == View::Update && !impl_->display.busy()) { impl_->draw_update(); impl_->overlay(); }
    }
    if (impl_->portal_live) impl_->portal_action = PortalAction::Stop;
    if (impl_->network_worker_id) {
        if (impl_->view != View::NetworkClosing) impl_->wait_network(SyncCancel);
        return;
    }
    const bool verifying = impl_->account_page == Impl::AccountPage::Verifying;
    impl_->cancel_sync();
    if (impl_->view == View::SyncSettings && !impl_->display.busy() && verifying) {
        impl_->account_notice = "Wi-Fi 已断开，账户没有保存。";
        impl_->draw_sync_settings(); impl_->overlay();
    }
    if (impl_->view == View::Sync && !impl_->display.busy() && impl_->sync_status != "Wi-Fi 已断开。") {
        impl_->sync_status = "Wi-Fi 已断开。";
        impl_->draw_sync(); impl_->overlay();
    }
    if (impl_->view == View::Portal) impl_->portal_action = PortalAction::Stop;
}
std::optional<WifiOperation> ReaderApp::take_wifi_request() {
    if (impl_->display.busy() || impl_->power_menu_wait ||
        impl_->view == View::PowerMenu || impl_->view == View::PowerOff) return std::nullopt;
    return std::exchange(impl_->wifi_request, std::nullopt);
}
void ReaderApp::complete_wifi(std::uint64_t id, const std::vector<provision::NearbyWifi>& nearby, const std::string& error,
                              LinkFailure failure) {
    if (!id || id != impl_->wifi_worker) return;
    impl_->wifi_worker = 0;
    switch (impl_->wifi_kind) {
    case WifiOperation::Kind::Scan:
        impl_->wifi_nearby = nearby;
        impl_->wifi_scanned = true;
        impl_->wifi_notice = error;
        break;
    case WifiOperation::Kind::Join:
        // A join that was cancelled after the link came up saves nothing.
        impl_->join_complete(error.empty() && !network::live(impl_->wifi_hold) ? "操作已取消。" : error, failure);
        network::release(impl_->wifi_hold);
        break;
    case WifiOperation::Kind::Connect:
        impl_->wifi_notice = error;
        if (!error.empty()) network::release(impl_->manual_hold);
        break;
    case WifiOperation::Kind::Clock:
        impl_->wifi_notice = !network::live(impl_->wifi_hold) ? "校准已取消。" :
                             error.empty() ? "时间已校准。" : error;
        network::release(impl_->wifi_hold);
        break;
    }
    if (impl_->view == View::WifiSettings && !impl_->display.busy()) { impl_->draw_wifi(); impl_->overlay(); }
}
void ReaderApp::show_wifi(const WifiStatus& status) {
    if (status.sequence <= impl_->wifi_status.sequence || impl_->display.busy()) return;
    impl_->wifi_status = status;
    if (status.phase == WifiPhase::Offline && !network::live(impl_->manual_hold) &&
        status.detail == "手动连接已暂停，点“立即连接”可恢复。") impl_->wifi_status.detail.clear();
    // A join saves its network, with the connection time, only when it completes; a cancelled or
    // failed join must leave network.toml as it was.
    if (status.phase == WifiPhase::Offline) impl_->join_link = false;
    if (status.phase == WifiPhase::Connected && !impl_->join_link) impl_->pending_wifi_record = status.ssid;
    if (status.phase == WifiPhase::Offline && impl_->wifi_notice == "正在结束…") impl_->wifi_notice.clear();
    // The join pages report their own result; link phases must not replace them.
    const bool joining = impl_->wifi_page == Impl::WifiPage::Joining || impl_->wifi_page == Impl::WifiPage::JoinFailed ||
                         impl_->wifi_page == Impl::WifiPage::Joined;
    if (impl_->view == View::WifiSettings && !joining) { impl_->draw_wifi(); impl_->overlay(); }
    // Panel frames and HTTPS steps are serialized, so the check page can follow its steps.
    if (impl_->view == View::Update && impl_->update_page == Impl::UpdatePage::Checking) { impl_->draw_update(); impl_->overlay(); }
    // Sync retains one busy frame until completion; network phase notifications must not drive the panel during TLS.
}
bool ReaderApp::portal_visible() const {
    return impl_->portal_live && (impl_->view == View::Portal || impl_->view == View::WifiSettings);
}
WifiTargets ReaderApp::upload_targets() const {
    std::string problem;
    return impl_->card_available ? impl_->wifi_targets(problem) : WifiTargets{};
}
bool ReaderApp::keyboard_visible() const { return impl_->view == View::TextEntry; }
bool ReaderApp::prepare_portal(bool station) {
    if (impl_->cancel_custom_preparation()) return false;
    if (impl_->portal_live || impl_->network_worker_id) return false;
    if (impl_->content_loading || impl_->usb_prepared || impl_->display.busy() || impl_->active_network ||
        !impl_->card_available || !impl_->persist() || !wifi_enabled()) return false;
    impl_->cancel_sync();
    impl_->clear_body_cache();
    impl_->stop_covers();
    impl_->clear_prefetch();
    impl_->release_counting();
    impl_->stop_counting_job();
    if (!impl_->portal_requested) ++impl_->portal_generation;
    impl_->portal_requested = false;
    impl_->portal_live = true;
    impl_->portal_open_wifi = false;
    impl_->portal_station = station;
    if (!impl_->portal_station) impl_->manual_target.reset();
    impl_->portal_started = false;
    impl_->portal = {};
    impl_->portal_closing = false;
    impl_->draw_portal();
    impl_->overlay();
    return true;
}
void ReaderApp::show_portal(const PortalStatus &status) {
    if (impl_->usb_prepared || impl_->display.busy()) return;
    if (!status.generation || status.generation != impl_->portal_generation ||
        (!impl_->portal_live && !impl_->portal_requested)) return;
    const auto view = impl_->view;
    if (status.phase == PortalPhase::Closed) {
        const bool requested = impl_->portal_requested;
        impl_->portal_live = impl_->portal_requested = false;
        const bool open_wifi = std::exchange(impl_->portal_open_wifi, false) || view == View::WifiSettings;
        if (impl_->portal_started && impl_->card_available) {
            // The session may have added fonts and lock screen images. A selected font that was
            // missing may now exist, so the next load opens it instead of keeping the fallback.
            impl_->scan_fonts();
            impl_->effective_font.clear();
            impl_->custom_prepare_finished = false;
        }
        if (view != View::Portal && view != View::WifiSettings && (!requested || view != impl_->portal_request_view)) return;
        if (open_wifi) {
            impl_->refresh_badge_config();
            if (impl_->card_available) {
                invalidate_identity_cache(); impl_->scan_library();
            }
            impl_->portal = {};
            impl_->portal_closing = false;
            impl_->portal_station = impl_->portal_started = false;
            impl_->message = status.detail;
            impl_->wifi_notice = status.detail;
            impl_->wifi_page = impl_->wifi_root = Impl::WifiPage::Home;
            impl_->draw_wifi(); impl_->overlay(); return;
        }
        impl_->refresh_badge_config();
        impl_->portal = {};
        impl_->portal_closing = false;
        impl_->message = status.detail;
        const bool uploaded = view == View::Portal;
        const bool started = impl_->portal_started;
        const bool station_failed = uploaded && impl_->portal_station && !status.detail.empty();
        impl_->portal_station = impl_->portal_started = false;
        if (!uploaded || !started || station_failed) {
            if (started && impl_->card_available) {
                invalidate_identity_cache();
                impl_->scan_library();
            }
            impl_->draw_import();
        } else if (impl_->card_available) {
            invalidate_identity_cache();
            impl_->scan_library(); impl_->draw_library();
        } else impl_->draw_settings();
        impl_->overlay();
        return;
    }
    if (view != View::Portal && view != View::WifiSettings) return;
    // The codes must be scannable under every refresh preset, so their first frame clears the panel.
    const bool codes_appear = impl_->portal.url.empty() && !status.url.empty();
    // With the same codes on screen only the summary between its rule and the button changes.
    const bool summary_only = view == View::Portal && !impl_->portal_closing && !status.url.empty() &&
        status.url == impl_->portal.url && status.network == impl_->portal.network &&
        status.access_ssid == impl_->portal.access_ssid && status.access_password == impl_->portal.access_password;
    // The first Waiting state may be superseded by Receiving before it is drawn.
    impl_->portal_started = impl_->portal_started || status.phase == PortalPhase::Waiting ||
        status.phase == PortalPhase::Receiving;
    impl_->portal = status;
    if (view == View::WifiSettings) impl_->draw_wifi();
    else impl_->draw_portal();
    if (summary_only) {
        const int top = impl_->portal_summary_line + 4;
        impl_->display.present({{"ui", 0, 0, ++impl_->ui_sequence}, {}, FramePurpose::Overlay,
            RefreshIntent::Content, RefreshRegion{0, top, 684, 1072 - top}});
    } else impl_->overlay(codes_appear);
}
bool ReaderApp::power_menu_pending() const { return impl_->power_menu_wait.has_value(); }
bool ReaderApp::show_power_menu() {
    const bool dictionary_view = impl_->view == View::Dictionary || impl_->view == View::DictionarySettings;
    if (dictionary_view && !impl_->network_worker_id && !impl_->active_network && !impl_->usb_prepared &&
        (impl_->content_loading || impl_->foreground_job)) {
        impl_->abandon_content();
        impl_->dictionary_notice = "查询已取消，可重试。";
        impl_->power_menu_wait = impl_->view;
        return true;
    }
    if (impl_->network_worker_id || impl_->active_network || impl_->content_loading || impl_->foreground_job ||
        impl_->usb_prepared || impl_->view == View::Portal || impl_->view == View::PowerOff) return false;
    if (impl_->display.busy()) {
        if (!impl_->sleep_wait_restore_pending) return false;
        impl_->power_menu_wait = impl_->view; return true;
    }
    if (impl_->view == View::PowerMenu) return true;
    cancel_sleep();
    if (impl_->display.busy()) { impl_->power_menu_wait = impl_->view; return true; }
    if (impl_->view == View::Dictionary) impl_->draw_dictionary();
    else if (impl_->view == View::DictionarySettings) impl_->draw_dictionary_settings();
    impl_->control(PowerMenu);
    return impl_->view == View::PowerMenu || impl_->power_menu_wait.has_value();
}
PowerRequest ReaderApp::take_power_request() {
    return std::exchange(impl_->power_request, PowerRequest{});
}
void ReaderApp::power_unavailable(const std::string& message) {
    if (impl_->display.busy()) return;
    impl_->note(message, 1012);
    impl_->overlay();
}
bool ReaderApp::prepare_power(PowerRequest request) {
    const bool install = request.install && request.action == PowerAction::Restart && !request.without_save &&
        impl_->view == View::Update && impl_->update_page == Impl::UpdatePage::Ready && impl_->update_ready;
    if (request.action == PowerAction::None || request.install != install ||
        (!install && impl_->view != View::PowerMenu) ||
        request.action != impl_->power_selected || impl_->display.busy() || impl_->active_network ||
        impl_->usb_prepared || (request.without_save && !impl_->power_save_failed)) return false;
    bool saved = !request.without_save && impl_->persist();
    std::string detail;
    if (saved && impl_->has_book() && impl_->session.visible_record() && impl_->card_available) {
        saved = impl_->store.save_last_book(impl_->book_path, detail) == StoreStatus::Ok;
        if (saved) { impl_->last_recorded = impl_->book_path; impl_->recent_save_failed = false; }
    }
    if (install) {
        if (const auto battery = impl_->update_battery_block(); !battery.empty()) {
            impl_->power_request = {}; impl_->power_selected = PowerAction::None;
            impl_->update_notice = battery;
            impl_->draw_update(); impl_->overlay();
            return false;
        }
        if (!saved) {
            impl_->power_request = {}; impl_->power_selected = PowerAction::None;
            impl_->update_notice = "阅读位置没有写入 TF 卡，没有重启。";
            impl_->draw_update(); impl_->overlay();
            return false;
        }
    }
    impl_->power_save_failed = !saved;
    impl_->power_progress_saved = saved;
    if (!saved && !request.without_save) {
        impl_->message = "阅读位置、书签或最近阅读记录未能保存到 TF 卡，仍要" +
            std::string(request.action == PowerAction::Restart ? "重启吗？" : "关机吗？");
        impl_->draw_power_menu(); impl_->overlay(); return false;
    }
    impl_->remember_position();
    impl_->cancel_sync();
    impl_->drain_save(); impl_->abandon_content(); impl_->foreground_job.reset();
    impl_->stop_covers(); impl_->clear_prefetch(); impl_->stop_counting_job();
    // Saving is the last cancellable step. Never restore a snapshot after teardown starts.
    impl_->power_return_canvas.reset(); impl_->power_return_hits.clear();
    impl_->power_request = {};
    if (install) impl_->draw_installing();
    else impl_->draw_power_off(saved ? "本地阅读位置已保存。" : "本地阅读位置未完整保存。");
    impl_->overlay(true);
    return true;
}
bool ReaderApp::close_power_files() {
    if (impl_->display.busy() || impl_->view != View::PowerOff) return false;
    impl_->document.close();
    impl_->ui.open_fallback(); impl_->body.open_fallback();
    impl_->library_font.close(); impl_->library_font_path.clear();
    impl_->library_uses_body = false;
    impl_->identities.clear(); impl_->font_scan.reset(); impl_->font_identity.clear();
    return true;
}
void ReaderApp::show_power_off_help() {
    if (impl_->display.busy() || impl_->view != View::PowerOff) return;
    impl_->draw_power_off(std::string(impl_->power_progress_saved ? "本地阅读位置已保存。" : "本地阅读位置未完整保存。") +
        "请长按电源键强制关机；若连接 USB，请拔下电源线。", true);
    impl_->overlay(true);
}
bool ReaderApp::begin_sleep() {
    if (impl_->display.busy() || impl_->power_menu_wait || impl_->usb_prepared ||
        impl_->view == View::PowerMenu || impl_->view == View::PowerOff || update_busy()) return false;
    impl_->sleep_onboarding = impl_->view == View::Onboarding;
    impl_->sleep_origin = impl_->origin_for(impl_->view);
    impl_->sleep_destination_captured = true;
    impl_->show_sleep_wait();
    return true;
}
bool ReaderApp::sleep_pending() const {
    return impl_->sleep_cover_deadline.has_value() || (impl_->sleep_destination_captured &&
        impl_->foreground_job && ContentJob::dictionary_kind(impl_->foreground_job->request.kind) &&
        impl_->foreground_job->started() && !impl_->foreground_job->done());
}
void ReaderApp::cancel_sleep() {
    impl_->sleep_destination_captured = false;
    impl_->custom_sleep_attempted = false;
    impl_->custom_prepare_finished = false;
    impl_->custom_sleep_image.reset();
    impl_->custom_sleep_deadline.reset();
    impl_->custom_rotation_name.clear();
    impl_->sleep_wait_restore_pending = bool(impl_->sleep_wait_footer);
    impl_->restore_sleep_wait();
    if (impl_->sleep_cover_deadline) {
        impl_->sleep_cover_deadline.reset();
        if (impl_->counting_job) impl_->counting_job->cancel();
    }
    if (const auto target = std::exchange(impl_->sleep_wifi_target, std::nullopt);
        target && impl_->card_available && impl_->wifi_enabled() && !impl_->usb_prepared && !impl_->portal_live) {
        // A released hold cannot be reused: the worker may already have completed its teardown.
        impl_->manual_hold = std::make_shared<std::atomic_bool>(false);
        impl_->wifi_worker = ++impl_->wifi_sequence;
        impl_->wifi_request = WifiOperation{WifiOperation::Kind::Connect, impl_->wifi_worker, *target, impl_->manual_hold};
    }
}
bool ReaderApp::prepare_sleep() {
    if (impl_->power_menu_wait) return false;
    if (impl_->foreground_job && ContentJob::dictionary_kind(impl_->foreground_job->request.kind) &&
        impl_->foreground_job->started() && !impl_->foreground_job->done()) {
        impl_->foreground_job->cancel(); return false;
    }
    bool cover_attempted = false;
    if (impl_->sleep_cover_deadline) {
        impl_->poll_counting_job();
        if (impl_->counting_job) {
            // Cancellation does not release files; completion must precede storage suspension.
            if (std::chrono::steady_clock::now() >= *impl_->sleep_cover_deadline)
                impl_->counting_job->cancel();
            return false;
        }
        impl_->sleep_cover_deadline.reset();
        cover_attempted = impl_->sleep_waiting_for_cover;
    }
    if (impl_->network_worker_id || impl_->portal_live || impl_->usb_prepared || impl_->view == View::Portal || impl_->view == View::PowerMenu || impl_->view == View::PowerOff) return false;
    if (impl_->display.busy() || impl_->active_network) return false;
    if (!impl_->sleep_destination_captured) {
        impl_->sleep_onboarding = impl_->view == View::Onboarding;
        impl_->sleep_origin = impl_->origin_for(impl_->view);
        impl_->sleep_destination_captured = true;
    }
    if (impl_->counting_job) {
        // Every background owner must return before cleanup or a sleep image can replace it.
        const auto kind = impl_->counting_job->request.kind;
        impl_->sleep_waiting_for_cover = kind == ContentJob::Kind::SleepCover;
        if (kind != ContentJob::Kind::SleepCover && kind != ContentJob::Kind::LockScreen)
            impl_->counting_job->cancel();
        if (kind == ContentJob::Kind::LockScreen && impl_->settings.sleep_screen == "custom") {
            if (!impl_->custom_sleep_deadline)
                impl_->custom_sleep_deadline = std::chrono::steady_clock::now() + kLockScreenPrepareBudget;
            impl_->sleep_cover_deadline = impl_->custom_sleep_deadline;
        } else impl_->sleep_cover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        return false;
    }
    invalidate_identity_cache();
    if (!impl_->persist()) return false;
    if (impl_->has_book() && !impl_->session.visible_record()) return false;
    impl_->remember_position();
    impl_->save_pages();
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    if (impl_->settings.sleep_screen == "custom" && !impl_->custom_sleep_attempted) {
        impl_->covers.clear(); impl_->make_image_room();
        impl_->prepare_custom_sleep(true);
        if (impl_->counting_job) {
            impl_->sleep_waiting_for_cover = false;
            impl_->sleep_cover_deadline = impl_->custom_sleep_deadline;
            return false;
        }
    }
    if (!cover_attempted && impl_->async_content && impl_->has_book() && !txt_name(impl_->book_path) &&
        impl_->settings.sleep_screen == "cover") {
        impl_->prepare_sleep_cover();
        if (impl_->counting_job) {
            impl_->sleep_waiting_for_cover = true;
            impl_->sleep_cover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            return false;
        }
    }
    const auto sync_state = impl_->sync_badge();
    impl_->cancel_sync();
    network::release(impl_->manual_hold);
    network::release(impl_->wifi_hold);
    // Password drafts never outlive the flow that entered them.
    impl_->leave_join(Impl::WifiPage::Home);
    std::fill(impl_->account.password.begin(), impl_->account.password.end(), '\0');
    impl_->account.password.clear();
    impl_->draw_sleep(sync_state);
    if (impl_->view != View::Sleeping) return false;
    impl_->overlay(true);
    return true;
}
void ReaderApp::wake(bool slept) {
    // A cancelled sleep must preserve failures raised while storage or display was returning.
    if (impl_->display.busy() || (!slept && impl_->view == View::Error))
        return;
    impl_->wifi_status.phase = WifiPhase::Offline;
    impl_->wifi_status.manual = false;
    impl_->wifi_status.ssid.clear(); impl_->wifi_status.detail.clear();
    impl_->wifi_notice.clear();
    impl_->custom_sleep_attempted = false;
    impl_->custom_prepare_finished = false;
    impl_->custom_sleep_deadline.reset();
    impl_->sleep_destination_captured = false;
    impl_->custom_notice_pending = slept && impl_->settings.sleep_screen == "custom" && !impl_->custom_notice.empty();
    if (slept) impl_->sleep_wifi_target.reset();
    else cancel_sleep();
    if (impl_->sleep_onboarding) {
        impl_->draw_onboarding(); impl_->overlay(true);
    } else if (impl_->sleep_origin == Impl::Origin::Reading && impl_->has_book()) {
        if (impl_->async_content) impl_->open(impl_->book_path);
        else impl_->recover_body();
    } else {
        impl_->return_to_origin(impl_->sleep_origin, true);
    }
}
void ReaderApp::card_removed() {
    stop_wifi();
    impl_->power_return_canvas.reset(); impl_->power_request = {};
    invalidate_identity_cache();
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    if (impl_->usb_prepared) { show_usb_transfer(false, "TF 卡已移除，请退出并重启。"); return; }
    impl_->cancel_sync();
    impl_->covers.clear();
    impl_->card_available = false;
    impl_->library_font.close();
    impl_->library_uses_body = false; impl_->library_font_path.clear();
    impl_->ui.open_fallback();
    impl_->body.open_fallback();
    impl_->effective_font.clear();
    if (impl_->view == View::Onboarding) {
        impl_->draw_onboarding(); impl_->overlay();
    } else impl_->error("TF 卡已移除。请重新插卡并重启设备。");
}
bool ReaderApp::save_due() const {
    return !impl_->portal_live && impl_->view != View::PowerMenu && impl_->view != View::PowerOff &&
           !impl_->usb_prepared && !impl_->display.busy() && (impl_->session.needs_save() ||
           (impl_->async_content && !impl_->recent_save_failed && impl_->session.visible_record() && impl_->last_recorded != impl_->book_path)) &&
           !impl_->save_failed && !impl_->save_job;
}
void ReaderApp::save_progress() {
    if (impl_->portal_live || impl_->usb_prepared || impl_->display.busy()) return;
    impl_->schedule_save();
}
bool ReaderApp::save_busy() const { return impl_->save_job || impl_->bookmark_job; }
std::optional<BookPages> ReaderApp::book_pages() const {
    const auto page = impl_->has_book() ? impl_->current_page() : 0;
    return page ? std::optional<BookPages>{{page, impl_->total_pages()}} : std::nullopt;
}
CacheUsage ReaderApp::cache_usage() const { return impl_->cache_usage(); }

bool ReaderApp::content_busy() const {
    return !!impl_->foreground_job || impl_->pending_content.has_value() ||
           (impl_->async_content && impl_->view == View::Reading && !impl_->image_viewing && impl_->body_images_pending);
}
bool ReaderApp::background_due() const {
    return !!impl_->cover_job || impl_->pending_covers.has_value() || !!impl_->counting_job ||
           impl_->chapter_cache_due() || impl_->custom_prepare_due() || impl_->prefetch_due() ||
           impl_->chapter_prefetch_due() || impl_->counting_due();
}
#if !defined(ESP_PLATFORM) || defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
// Only the diagnostic console reads this; other firmware builds do not carry the name table.
const char *ReaderApp::sync_state() const {
    static constexpr const char *names[] = {"unchecked", "no-record", "same", "different", "unmapped", "uploaded",
                                            "uncertain", "unauthorized", "failed", "link-failed", "config-changed"};
    return names[static_cast<unsigned>(impl_->sync_state)];
}
int ReaderApp::sync_order() const { return impl_->sync_state == Impl::SyncState::Different ? impl_->further() : 0; }
int ReaderApp::font_size_px() const { return impl_->settings.font_size_px; }
#endif
const char *ReaderApp::background_state() const {
    if (impl_->inline_image_pending || (impl_->view == View::Reading && !impl_->image_viewing && impl_->body_images_pending)) return "image";
    if (impl_->content_loading) {
        const auto kind = impl_->pending_content ? impl_->pending_content->kind : impl_->foreground_job->request.kind;
        return kind == ContentJob::Kind::Open ? "opening" : kind == ContentJob::Kind::Inspect ? "details" :
            kind == ContentJob::Kind::MapUpload || kind == ContentJob::Kind::MapDownload ? "mapping" :
            kind == ContentJob::Kind::Chapter ? "chapter" : "image";
    }
    if (impl_->cover_job || impl_->pending_covers) return "covers";
    if (impl_->counting_job && impl_->counting_job->request.kind == ContentJob::Kind::Chapter) return "chapter-prefetch";
    if (impl_->counting_job && (impl_->counting_job->request.kind == ContentJob::Kind::SleepCover || impl_->counting_job->request.kind == ContentJob::Kind::LockScreen)) return "sleep-cover";
    if (impl_->prefetch_due()) return "prefetch";
    if (impl_->counting_blocked == impl_->chapter_index) return "waiting";
    if ((!impl_->pages_cache_disabled && impl_->pages_complete && impl_->font_identity.empty()) ||
        (impl_->counting_job && impl_->counting_job->request.kind == ContentJob::Kind::FontIdentity))
        return "font-identity";
    if (!impl_->pages_cache_disabled && !impl_->font_identity.empty() &&
        (impl_->pages_cache_pending || impl_->pages_dirty)) return "page-cache";
    if (!impl_->has_book() || impl_->pages_complete) return "idle";
    if (impl_->pages_failed) return "failed";
    return "counting";
}
void ReaderApp::poll_background(bool cancel) {
    if (impl_->view == View::PowerOff) return;
    if (cancel && impl_->body_images_pending && !impl_->inline_image_pending && !impl_->display.busy())
        impl_->inline_image_after = std::chrono::steady_clock::now() + inline_image_delay;
    if (cancel && impl_->counting_job) impl_->counting_job->cancel();
    impl_->poll_counting_job();
    if (impl_->sleep_wait_restore_pending) impl_->restore_sleep_wait();
    // Keep completed background work from replacing the sleep feedback before preparation.
    if (impl_->sleep_wait_footer && !impl_->sleep_wait_restore_pending && impl_->view == impl_->sleep_wait_view) return;
    if (!cancel || !impl_->dictionary_contact) impl_->flush_dictionary_selection();
    if (impl_->clear_cache_pending) {
        if (impl_->view != View::StorageSettings || !impl_->card_available) impl_->clear_cache_pending = false;
        else if (!cancel && !impl_->counting_job && !impl_->display.busy() && !impl_->save_job && !impl_->bookmark_job)
            impl_->control(ClearCache);
    }
    // The counting session lives only while the reading page is shown.
    if (impl_->view != View::Reading && !impl_->counting_job) impl_->counting_session.reset();
    impl_->poll_covers();
    impl_->poll_save();
    impl_->poll_bookmark();
    if (!cancel) impl_->reclaim_caches();
    if (impl_->save_error_pending && impl_->view != View::PowerMenu && !impl_->display.busy() && !impl_->content_loading) {
        impl_->save_error_pending = false; impl_->error(impl_->message);
    }
    if (!impl_->bookmark_error.empty() && impl_->view != View::PowerMenu && impl_->view != View::PowerOff &&
        !impl_->display.busy() && !impl_->content_loading) {
        impl_->error(std::exchange(impl_->bookmark_error, std::string()));
    }
    if (!cancel || !impl_->content_loading) impl_->poll_content(!cancel);
    if (!cancel) impl_->poll_dictionary_panel();
    // Waits for the queued taps of one burst, so they reach the panel in a single update.
    if (!cancel) impl_->flush_entry();
    if (!cancel && impl_->custom_notice_pending &&
        !impl_->sleep_cover_deadline && !impl_->display.busy() && !impl_->content_loading &&
        !impl_->save_failed && !impl_->recent_save_failed &&
        (impl_->view == View::Library || impl_->view == View::Reading || impl_->view == View::Settings)) {
        impl_->custom_notice_pending = false;
#ifdef ESP_PLATFORM
        ESP_LOGW("reader_app", "Sleep image notice: %s", impl_->custom_notice.c_str());
#endif
        impl_->message = kSleepFallbackNotice;
        // Optional sleep imagery must not cancel body image work or replace the wake destination.
        if (impl_->view == View::Reading) impl_->footer();
        else if (impl_->view == View::Library) impl_->footer_bar(impl_->notice(), impl_->library_page, impl_->library_pages());
        else impl_->footer_bar(impl_->notice());
        const int top = impl_->view == View::Reading ? footer_top : Impl::footer_zone;
        impl_->display.present({{"ui", 0, 0, ++impl_->ui_sequence}, {}, FramePurpose::Overlay,
            RefreshIntent::Content, RefreshRegion{0, top, 684, 1216 - top}});
    }
    if (!cancel && !impl_->pending_wifi_record.empty() && impl_->card_available &&
        !impl_->portal_live && !impl_->usb_prepared && !impl_->network_worker_id &&
        !impl_->display.busy() && !impl_->save_job && !impl_->bookmark_job && !impl_->foreground_job) {
        std::string error;
        const auto name = std::exchange(impl_->pending_wifi_record, std::string());
        if (!provision::save_connection(impl_->root, name, std::time(nullptr), error))
            impl_->wifi_notice = "连接记录未保存，请检查 TF 卡。";
    }
    if (impl_->power_menu_wait) {
        if (impl_->view != *impl_->power_menu_wait || !impl_->card_available) impl_->power_menu_wait.reset();
        else if (!cancel && !impl_->counting_job && !impl_->display.busy() && !impl_->save_job &&
                 !impl_->bookmark_job && !impl_->foreground_job && !impl_->content_loading) {
            impl_->power_menu_wait.reset();
            show_power_menu();
        }
    }
    // The first body frame permits image preparation immediately; subsequent pages retain the dwell interval.
    if (!cancel && !impl_->sleep_cover_deadline && !impl_->power_menu_wait && impl_->async_content && impl_->body_images_pending && !impl_->inline_image_pending &&
        std::chrono::steady_clock::now() >= impl_->inline_image_after &&
        !impl_->content_loading && !impl_->foreground_job && !impl_->pending_content &&
        impl_->view == View::Reading && !impl_->image_viewing && !impl_->display.busy() &&
        impl_->has_book() && impl_->card_available && !impl_->usb_prepared && !impl_->page.images.empty())
        impl_->begin_image(impl_->page.start, impl_->page, false, true);
}
void ReaderApp::background_step(const std::function<bool()>& stop) {
    if (impl_->view == View::PowerOff) return;
    impl_->poll_counting_job();
    if (stop && stop()) { if (impl_->counting_job) impl_->counting_job->cancel(); return; }
    if (impl_->chapter_cache_due()) impl_->write_chapter_cache();
    else if (impl_->custom_prepare_due()) impl_->prepare_custom_sleep(false);
    else if (impl_->prefetch_due()) impl_->prepare_next(stop);
    else if (impl_->chapter_prefetch_due()) impl_->prepare_chapter();
    else if (impl_->counting_due()) impl_->count_pages(stop);
}
void ReaderApp::show_error(const std::string &message) {
    if (!impl_->display.busy())
        impl_->error(message);
}
std::optional<NetworkOperation> ReaderApp::take_network_request() {
    if (impl_->display.busy())
        return std::nullopt;
    const bool account = impl_->view == View::SyncSettings && impl_->account_check;
    if (impl_->network_worker_id || (impl_->view != View::Sync && !account) || !impl_->active_network ||
        !impl_->network_request || impl_->network_request->id != impl_->active_network)
        return std::nullopt;
    if (!wifi_enabled()) {
        impl_->cancel_sync();
        if (account) impl_->draw_sync_settings(); else impl_->draw_sync();
        impl_->overlay();
        return std::nullopt;
    }
    impl_->network_worker_id = impl_->active_network;
    auto request = std::move(impl_->network_request);
    impl_->network_request.reset();
    return request;
}
void ReaderApp::complete_network(std::uint64_t id, int status, const std::string &body,
                                 const std::string &error, LinkFailure failure) {
    if (id == impl_->network_worker_id && id) {
        impl_->network_worker_id = 0;
        if (impl_->network_after_cancel) {
            const int action = std::exchange(impl_->network_after_cancel, 0);
            if (impl_->view == View::NetworkClosing) {
                impl_->view = impl_->network_return_view;
                if (impl_->view == View::SyncSettings) { impl_->draw_sync_settings(); impl_->overlay(); }
                else if (action == SyncCancel) { impl_->draw_sync(); impl_->overlay(); }
                else impl_->control(action);
            }
            return;
        }
    }
    if (!impl_->usb_prepared) impl_->network_complete(id, status, body, error, failure);
}
} // namespace pico_page
