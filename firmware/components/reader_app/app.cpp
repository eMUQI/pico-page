#include "app_impl.h"
#include "pico_page/clock_time.h"

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
    if (!okay) message = tr(kSaveFailed);
    if (job->result.last_book_saved) last_recorded = job->request.relative_path;
    else if (okay && job->request.save_last_book) {
        recent_save_failed = true; message = tr("最近阅读记录未保存");
    }
}

void ReaderApp::Impl::apply_typography() {
    if (!bookmark_io_ready()) return;
    const bool mode_changed = draft.original_epub != settings.original_epub;
    if (mode_changed && (!book_scope || !typography_preview)) return;
    std::string detail;
    if (font_fallback || weight_unavailable) {
        error(font_fallback ? tr("所选字体不可用，排版未保存") : tr("所选字重不可用，排版未保存"));
        return;
    }
    auto saved_values = draft;
    if (!book_scope) {
        const auto& defaults = edit_snapshot.global_settings;
        if (txt_name(book_path) || !draft.original_epub)
            saved_values.original_weight_adjustment = defaults.original_weight_adjustment;
        if (txt_name(book_path)) {
            saved_values.original_epub = defaults.original_epub;
        } else if (draft.original_epub) {
            saved_values.justify = defaults.justify;
            saved_values.font_weight = defaults.font_weight;
            saved_values.line_height = defaults.line_height;
            saved_values.paragraph_gap_lines = defaults.paragraph_gap_lines;
            saved_values.indent_chars = defaults.indent_chars;
        }
    }
    const auto result = reset_overrides && book_scope
                            ? store.clear_book_settings(edit_snapshot, detail)
                            : store.apply_settings(edit_snapshot, saved_values, book_scope, detail);
    if (result != StoreStatus::Ok) {
        error(trf("排版未保存。{reason}。", {{"reason", reason(detail)}}));
        return;
    }
    const auto effective = store.load_settings(book_path).settings;
    const bool layout_changed = effective.original_epub != settings.original_epub ||
        effective.txt_reflow != settings.txt_reflow || effective.fullscreen_reading != settings.fullscreen_reading ||
        effective.original_weight_adjustment != settings.original_weight_adjustment || effective.justify != settings.justify ||
        effective.font != settings.font ||
        effective.font_weight != settings.font_weight ||
        effective.font_size_px != settings.font_size_px ||
        effective.line_height != settings.line_height ||
        effective.margin_x_px != settings.margin_x_px ||
        effective.paragraph_gap_lines != settings.paragraph_gap_lines ||
        effective.indent_chars != settings.indent_chars;
    settings = effective;
    ContentCacheEntry applied_cache;
    if (mode_changed) {
        chapter = std::move(typography_preview->chapter);
        author_fonts = std::move(typography_preview->fonts);
        author_fonts_stable = typography_preview->fonts_stable; author_fonts_released = false;
        applied_cache = std::move(typography_preview->cache);
        offset = typography_preview_offset;
    }
    typography_preview.reset();
    if (!book_scope) {
        load_library_font(store.load_settings().settings.font);
        message = layout_changed ? tr("阅读默认已保存，当前书已应用") : tr("阅读默认已保存，本书排版未变");
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
    auto reader = mode_changed ? std::move(chapter_session) : nullptr;
    const auto reader_bytes = chapter_stream_bytes;
    abandon_content(mode_changed ? ChapterCacheRetention::Discard : ChapterCacheRetention::Keep);
    adopt_chapter_cache(std::move(applied_cache));
    if (reader) { chapter_session = std::move(reader); chapter_stream_bytes = reader_bytes; }
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
        error(trf("刷新设置未保存。{reason}。", {{"reason", reason(detail)}}));
        return;
    }
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
bool ReaderApp::set_diagnostic_transfer(bool active) {
    if (impl_->statistics_serial_transfer == active) return true;
    if (active) {
        if (!impl_->statistics_worker_idle()) return false;
        impl_->pause_statistics();
        invalidate_identity_cache();
        impl_->persist();
    }
    impl_->statistics_serial_transfer = active;
    if (!active) impl_->resume_statistics();
    return true;
}

void ReaderApp::invalidate_identity_cache() {
    impl_->library_font_reload = true;
    impl_->drain_save(); impl_->abandon_content(); impl_->foreground_job.reset(); impl_->stop_covers();
    impl_->clear_prefetch(); impl_->stop_counting_job(); impl_->identities.clear();
    impl_->font_identity.clear(); impl_->font_scan.reset(); impl_->effective_font.clear();
    impl_->reset_dictionary_contact();
    impl_->dictionary_session.reset(); impl_->dictionary_definition.reset();
    impl_->dictionary_failed_sources.clear();
    impl_->dictionary_pages.clear(); impl_->dictionary_page = {};
}
ReaderApp::~ReaderApp() {
    network::release(impl_->manual_hold);
    network::release(impl_->wifi_hold);
    impl_->cancel_sync();
    impl_->drain_save();
}
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
    if (allow_refresh && !impl_->settings.fullscreen_reading && impl_->footer_charging != charging &&
        impl_->view == View::Reading &&
        impl_->footer_device == reading_footer::Device::Battery &&
        !impl_->display.busy() && !impl_->content_loading && !impl_->image_viewing &&
        !impl_->sleep_wait_footer && !impl_->sleep_wait_restore_pending && !impl_->sleep_destination_captured) {
        impl_->footer();
        impl_->display.present({{"ui", 0, 0, ++impl_->ui_sequence}, {}, FramePurpose::Overlay,
            RefreshIntent::Content, RefreshRegion{0, footer_top, 684, 1216 - footer_top}});
    }
}
void ReaderApp::set_firmware_version(const std::string& version) { impl_->running_version = version; }
void ReaderApp::set_firmware_layout_supported(bool supported) { impl_->update_layout_supported = supported; }
std::optional<UpdateOperation> ReaderApp::take_update_request() {
    if (impl_->display.busy() || impl_->update_worker_id || !impl_->update_request) return std::nullopt;
    impl_->update_worker_id = impl_->update_request->id;
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
    if (event.event.phase == Phase::Begin) impl_->statistics_waiting_control.reset();
    const bool reading = impl_->statistics_running && event.event.phase == Phase::Begin;
    if (reading) impl_->pause_statistics();
    impl_->input(event);
    if (reading && !impl_->pending) impl_->resume_statistics();
}
bool ReaderApp::can_long_press_at(int x, int y) const {
    if (impl_->image_at(x, y)) return true;
    if (impl_->view == View::Reading && !impl_->image_viewing && y < impl_->reading_touch_bottom() &&
        graphics::text_hit(impl_->page, x, y)) return true;
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
             job->request.kind != ContentJob::Kind::Chapter && !ContentJob::dictionary_kind(job->request.kind) &&
             !(job->request.statistics && job->request.statistics->kind == StatisticsWork::Kind::Query)) || job->pause();
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
        impl_->confirm_statistics_body(frame);
        if (frame.purpose == FramePurpose::Body) impl_->remember_page();
        impl_->confirmed_link_return = impl_->link_return;
        impl_->confirmed_sync_return = impl_->sync_return;
        impl_->save_failed = false;
        if (!impl_->async_content && !impl_->recent_save_failed && impl_->last_recorded != impl_->book_path) {
            std::string error;
            if (impl_->store.save_last_book(impl_->book_path, error) != StoreStatus::Ok) {
                impl_->recent_save_failed = true;
                impl_->message = impl_->tr("最近阅读记录未保存");
            } else
                impl_->last_recorded = impl_->book_path;
        }
    }
    if (success && impl_->view == View::Tools && std::exchange(impl_->tools_save_pending, false)) {
        impl_->schedule_save();
    }
    if (auto work = std::exchange(impl_->deferred, nullptr); work && success)
        work();
    if (impl_->view == View::PowerOff) return;
    if (success && impl_->sleep_wait_restore_pending) {
        if (impl_->sleep_wait_footer) impl_->restore_sleep_wait();
        else impl_->sleep_wait_restore_pending = false;
    }
    if (success && frame.purpose != FramePurpose::Body) impl_->resume_statistics();
    if (!success) {
        impl_->statistics_confirmed = false;
        impl_->abandon_content();
        if (impl_->usb_prepared) {
            impl_->message = impl_->tr("屏幕更新失败，请退出并重启。");
            impl_->draw_usb(); impl_->overlay(true);
        } else if (impl_->view == View::PowerMenu && impl_->power_return_canvas) {
            impl_->close_power_menu();
        } else {
            const auto origin = impl_->origin_for(impl_->view);
            const bool return_help = (impl_->view == View::Onboarding && impl_->onboarding_from_help) ||
                (impl_->view == View::Error && impl_->error_onboarding_help);
            const auto destination = return_help ? impl_->tr("回到使用说明。") : origin == Impl::Origin::Reading ? impl_->tr("恢复已确认位置。") :
                                     origin == Impl::Origin::Settings ? impl_->tr("回到设置首页。") : impl_->tr("回到书库。");
            impl_->error(frame.region
                ? impl_->trf("屏幕局部更新失败。返回将{destination}", {{"destination", destination}})
                : impl_->trf("屏幕更新失败。返回将{destination}", {{"destination", destination}}));
        }
    }
}
View ReaderApp::view() const {
    return impl_->view;
}
const std::string &ReaderApp::status() const {
    return impl_->current_notice();
}
const std::optional<ReadingRecord> &ReaderApp::saved_record() const {
    return impl_->session.saved_record();
}
bool ReaderApp::fast_page_updates() const {
    return impl_->settings.refresh_preset == "fast";
}
bool ReaderApp::has_reading_footer() const { return !impl_->settings.fullscreen_reading; }
PageEffect ReaderApp::page_effect() const { return impl_->page_effect; }
void ReaderApp::set_page_effect(PageEffect effect) {
    impl_->page_effect = effect;
    if (impl_->view == View::PageEffects) {
        impl_->draw_page_effects();
        if (!impl_->display.busy()) impl_->overlay();
    }
}
i18n::Language ReaderApp::language() const { return impl_->settings.language; }
int ReaderApp::auto_sleep_minutes() const {
    return impl_->settings.auto_sleep_minutes;
}
UsbTransferAction ReaderApp::take_usb_transfer_action() {
    if (impl_->usb_action == UsbTransferAction::Start) {
        if (impl_->usb_request_view && impl_->view != *impl_->usb_request_view) impl_->usb_action = UsbTransferAction::None;
        else if (impl_->cancel_custom_preparation() || !impl_->statistics_worker_idle()) return UsbTransferAction::None;
        else if (impl_->statistics_collector.pending_count() && !impl_->save_failed && impl_->session.needs_save()) {
            impl_->schedule_save();
            return UsbTransferAction::None;
        } else if (!impl_->save_failed && !impl_->flush_statistics_for_handoff()) return UsbTransferAction::None;
    }
    impl_->usb_request_view.reset();
    return std::exchange(impl_->usb_action, UsbTransferAction::None);
}
bool ReaderApp::prepare_usb_transfer(std::string* problem) {
    const auto refuse = [&](const char* reason) {
        if (problem) *problem = reason;
        return false;
    };
    if (impl_->foreground_job && ContentJob::dictionary_kind(impl_->foreground_job->request.kind) &&
        impl_->foreground_job->started() && !impl_->foreground_job->done()) {
        impl_->foreground_job->cancel(); return refuse(impl_->tr("正在结束后台任务，请稍后重试。"));
    }
    if (impl_->cancel_custom_preparation()) return refuse(impl_->tr("正在结束后台任务，请稍后重试。"));
    if (!impl_->statistics_worker_idle()) return refuse(impl_->tr("正在结束后台任务，请稍后重试。"));
    invalidate_identity_cache();
    if (!impl_->card_available) return refuse(impl_->tr("没有可用的 TF 卡。"));
    if (impl_->network_worker_id || impl_->portal_live || impl_->usb_prepared || impl_->display.busy() || impl_->active_network)
        return refuse(impl_->tr("设备正忙，请稍后重试。"));
    if (!impl_->persist()) return refuse(impl_->tr("阅读位置没有保存，没有进入 USB 传输。请稍后重试。"));
    if (!impl_->flush_statistics_for_handoff()) return refuse(impl_->tr("正在结束后台任务，请稍后重试。"));
    save_clock_sync();
    impl_->cancel_sync();
    impl_->save_pages();
    if (!impl_->ui.open_fallback()) return refuse(impl_->tr("准备失败，请稍后重试。"));
    impl_->library_font.close();
    impl_->library_uses_body = false; impl_->library_font_path.clear();
    impl_->document.close();
    impl_->release_author_fonts();
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
    impl_->retire_statistics_mount();
    impl_->card_available = false;
    impl_->usb_prepared = true;
    return true;
}
void ReaderApp::show_usb_transfer(bool active, const std::string& error) {
    impl_->message = impl_->tr(error.c_str());
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
        } else if (impl_->cancel_custom_preparation() || !impl_->statistics_worker_idle()) return PortalAction::None;
    }
    return std::exchange(impl_->portal_action, PortalAction::None);
}
std::uint64_t ReaderApp::portal_generation() const { return impl_->portal_generation; }
bool ReaderApp::wifi_enabled() const { return impl_->card_available && impl_->wifi_enabled(); }
void ReaderApp::record_clock_sync(std::int64_t utc_seconds) {
    if (!clock_time::valid(utc_seconds)) return;
    impl_->last_clock_sync_at = utc_seconds;
    impl_->pending_clock_sync_at = utc_seconds;
}
void ReaderApp::save_clock_sync() {
    if (!impl_->pending_clock_sync_at || !impl_->card_available || impl_->usb_prepared || impl_->portal_live ||
        impl_->display.busy() || impl_->save_job || impl_->bookmark_job || impl_->foreground_job ||
        impl_->counting_job || impl_->cover_job) return;
    const auto utc_seconds = std::exchange(impl_->pending_clock_sync_at, 0);
    [[maybe_unused]] const auto status = impl_->store.save_clock_sync(utc_seconds);
#ifdef ESP_PLATFORM
    if (status != StoreStatus::Ok)
        ESP_LOGW("reader_app", "Last clock sync not saved (store status %d)", static_cast<int>(status));
#endif
}
void ReaderApp::stop_wifi(bool for_sleep) {
    if (!for_sleep) impl_->sleep_wifi_target.reset();
    else if (network::live(impl_->manual_hold) && impl_->manual_target)
        impl_->sleep_wifi_target = impl_->manual_target;
    network::release(impl_->manual_hold);
    network::release(impl_->sync_hold);
    const bool cancelling_clock = impl_->clock_running() && network::live(impl_->wifi_hold);
    network::release(impl_->wifi_hold);
    if (impl_->wifi_request) {
        if (impl_->clock_running()) {
            impl_->wifi_notice = impl_->tr("校准已取消。");
            if (impl_->clock_from_sync) {
                impl_->clock_feedback = Impl::ClockFeedback::Cancelled;
                impl_->clock_notice = impl_->tr("校准已取消，原时间保持不变。");
            }
        }
        impl_->wifi_request.reset(); impl_->wifi_worker = 0;
    }
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
        impl_->account_notice = impl_->tr("Wi-Fi 已断开，账户没有保存。");
        impl_->draw_sync_settings(); impl_->overlay();
    }
    if (impl_->view == View::Sync && !impl_->display.busy() && !impl_->sleep_wait_footer) {
        if (impl_->sync_subtab == Impl::SyncSubtab::Clock) {
            if (cancelling_clock) { impl_->draw_sync(); impl_->overlay(); }
        } else if (impl_->sync_status != impl_->tr("Wi-Fi 已断开。")) {
            impl_->sync_status = impl_->tr("Wi-Fi 已断开。");
            impl_->draw_sync(); impl_->overlay();
        }
    }
    if (impl_->view == View::Portal) impl_->portal_action = PortalAction::Stop;
}
std::optional<WifiOperation> ReaderApp::take_wifi_request() {
    if (impl_->display.busy() || impl_->power_menu_wait ||
        impl_->view == View::PowerMenu || impl_->view == View::PowerOff) return std::nullopt;
    return std::exchange(impl_->wifi_request, std::nullopt);
}
void ReaderApp::complete_wifi(std::uint64_t id, const std::vector<provision::NearbyWifi>& nearby, const std::string& error,
                              LinkFailure failure, bool clock_calibrated) {
    if (!id || id != impl_->wifi_worker) return;
    impl_->wifi_worker = 0;
    switch (impl_->wifi_kind) {
    case WifiOperation::Kind::Scan:
        impl_->wifi_nearby = nearby;
        impl_->wifi_scanned = true;
        impl_->wifi_notice = impl_->tr(error.c_str());
        break;
    case WifiOperation::Kind::Join:
        impl_->join_complete(error.empty() && !network::live(impl_->wifi_hold) ? impl_->tr("操作已取消。") : std::string(impl_->tr(error.c_str())), failure);
        network::release(impl_->wifi_hold);
        break;
    case WifiOperation::Kind::Connect:
        impl_->wifi_notice = impl_->tr(error.c_str());
        if (!error.empty()) network::release(impl_->manual_hold);
        break;
    case WifiOperation::Kind::Clock: {
        const bool cancelled = !clock_calibrated && !network::live(impl_->wifi_hold);
        const bool completed = !cancelled && (clock_calibrated || error.empty());
        impl_->wifi_notice = cancelled ? impl_->tr("校准已取消。")
            : completed ? impl_->tr("时间已校准。") : impl_->tr(error.c_str());
        if (impl_->clock_from_sync) {
            impl_->clock_feedback = cancelled ? Impl::ClockFeedback::Cancelled
                : completed ? Impl::ClockFeedback::Success : Impl::ClockFeedback::Failed;
            impl_->clock_notice = cancelled ? impl_->tr("校准已取消，原时间保持不变。") : impl_->wifi_notice;
        }
        network::release(impl_->wifi_hold);
        break;
    }
    }
    if (impl_->display.busy() || impl_->sleep_wait_footer) return;
    if (impl_->view == View::WifiSettings) { impl_->draw_wifi(); impl_->overlay(); }
    else if (impl_->view == View::Sync) { impl_->draw_sync(); impl_->overlay(); }
}
void ReaderApp::show_wifi(const WifiStatus& status) {
    if (status.sequence <= impl_->wifi_status.sequence || impl_->display.busy()) return;
    impl_->wifi_status = status;
    if (status.phase == WifiPhase::Offline && !network::live(impl_->manual_hold) &&
        status.detail == "手动连接已暂停，点「立即连接」可恢复。") impl_->wifi_status.detail.clear();
    if (status.phase == WifiPhase::Offline) impl_->join_link = false;
    if (status.phase == WifiPhase::Connected && !impl_->join_link) impl_->pending_wifi_record = status.ssid;
    if (status.phase == WifiPhase::Offline && impl_->wifi_notice == impl_->tr("正在结束…")) impl_->wifi_notice.clear();
    const bool joining = impl_->wifi_page == Impl::WifiPage::Joining || impl_->wifi_page == Impl::WifiPage::JoinFailed ||
                         impl_->wifi_page == Impl::WifiPage::Joined;
    if (impl_->view == View::WifiSettings && !joining && !impl_->clock_running()) { impl_->draw_wifi(); impl_->overlay(); }
    if (impl_->view == View::Update && impl_->update_page == Impl::UpdatePage::Checking) { impl_->draw_update(); impl_->overlay(); }
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
    if (impl_->cancel_custom_preparation() || !impl_->statistics_worker_idle()) return false;
    if (impl_->portal_live || impl_->network_worker_id) return false;
    if (impl_->content_loading || impl_->usb_prepared || impl_->display.busy() || impl_->active_network ||
        !impl_->card_available || !impl_->persist() || !wifi_enabled()) return false;
    impl_->cancel_sync();
    impl_->clear_body_cache();
    impl_->stop_covers();
    impl_->clear_prefetch();
    impl_->release_counting();
    impl_->stop_counting_job();
    impl_->pause_statistics();
    if (!impl_->portal_requested) ++impl_->portal_generation;
    impl_->portal_requested = false;
    impl_->portal_live = true;
    impl_->portal_open_wifi = false;
    impl_->portal_station = station;
    if (!impl_->portal_station) impl_->manual_target.reset();
    impl_->portal_ready_received = false;
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
        if (impl_->portal_ready_received && impl_->card_available) {
            StatisticsOptions options;
            if (impl_->store.load_statistics_options(options) != StoreStatus::Ok) options.enabled = false;
            impl_->adopt_statistics_options(options);
            impl_->scan_fonts();
            impl_->effective_font.clear();
            impl_->custom_prepare_finished = false; impl_->custom_prepare_attempts = 0;
        }
        if (view != View::Portal && view != View::WifiSettings && (!requested || view != impl_->portal_request_view)) return;
        if (open_wifi) {
            impl_->refresh_badge_config();
            if (impl_->card_available) {
                invalidate_identity_cache(); impl_->scan_library();
            }
            impl_->portal = {};
            impl_->portal_closing = false;
            impl_->portal_station = impl_->portal_ready_received = false;
            impl_->message = impl_->tr(status.detail.c_str());
            impl_->wifi_notice = impl_->tr(status.detail.c_str());
            impl_->wifi_page = impl_->wifi_entry_page = Impl::WifiPage::Home;
            impl_->draw_wifi(); impl_->overlay(); return;
        }
        impl_->refresh_badge_config();
        impl_->portal = {};
        impl_->portal_closing = false;
        impl_->message = impl_->tr(status.detail.c_str());
        const bool uploaded = view == View::Portal;
        const bool started = impl_->portal_ready_received;
        const bool station_failed = uploaded && impl_->portal_station && !status.detail.empty();
        impl_->portal_station = impl_->portal_ready_received = false;
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
    const bool codes_appear = impl_->portal.url.empty() && !status.url.empty();
    const bool summary_only = view == View::Portal && !impl_->portal_closing && !status.url.empty() &&
        status.url == impl_->portal.url && status.network == impl_->portal.network &&
        status.access_ssid == impl_->portal.access_ssid && status.access_password == impl_->portal.access_password;
    impl_->portal_ready_received = impl_->portal_ready_received || status.phase == PortalPhase::Waiting ||
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
    if (impl_->counting_job && impl_->counting_job->request.statistics) {
        impl_->pause_statistics();
        impl_->counting_job->cancel();
        impl_->power_menu_wait = impl_->view;
        return true;
    }
    const bool dictionary_view = impl_->view == View::Dictionary || impl_->view == View::DictionarySettings;
    if (dictionary_view && !impl_->network_worker_id && !impl_->active_network && !impl_->usb_prepared &&
        (impl_->content_loading || impl_->foreground_job)) {
        impl_->abandon_content();
        impl_->dictionary_notice = impl_->tr("查询已取消，可重试。");
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
    if (impl_->power_request.action != PowerAction::None && !impl_->statistics_worker_idle()) return {};
    return std::exchange(impl_->power_request, PowerRequest{});
}
void ReaderApp::power_unavailable(const std::string& message) {
    if (impl_->display.busy()) return;
    impl_->note(impl_->tr(message.c_str()), 1012);
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
    std::string unsaved = saved || request.without_save ? "" : impl_->session.needs_save() ? impl_->tr("阅读位置") : impl_->tr("书签");
    std::string detail;
    const bool reading = impl_->has_book() && impl_->session.visible_record() && impl_->card_available;
    if (saved && reading) {
        saved = impl_->store.save_last_book(impl_->book_path, detail) == StoreStatus::Ok;
        if (saved) { impl_->last_recorded = impl_->book_path; impl_->recent_save_failed = false; }
        else unsaved = impl_->tr("最近阅读记录");
    }
    impl_->power_reading = reading;
    if (install) {
        if (const auto battery = impl_->update_battery_block(); !battery.empty()) {
            impl_->power_request = {}; impl_->power_selected = PowerAction::None;
            impl_->update_notice = battery;
            impl_->draw_update(); impl_->overlay();
            return false;
        }
        if (!saved) {
            impl_->power_request = {}; impl_->power_selected = PowerAction::None;
            impl_->update_notice = impl_->trf("{data}没有保存到 TF 卡，没有重启。", {{"data", unsaved}});
            impl_->draw_update(); impl_->overlay();
            return false;
        }
    }
    impl_->power_save_failed = !saved;
    impl_->power_progress_saved = saved;
    if (!saved && !request.without_save) {
        impl_->message = request.action == PowerAction::Restart
            ? impl_->trf("{data}没有保存到 TF 卡，仍要重启吗？", {{"data", unsaved}})
            : impl_->trf("{data}没有保存到 TF 卡，仍要关机吗？", {{"data", unsaved}});
        impl_->draw_power_menu(); impl_->overlay(); return false;
    }
    if (saved && !impl_->flush_statistics_for_handoff()) {
        impl_->power_request = request;
        return false;
    }
    impl_->remember_position();
    impl_->cancel_sync();
    impl_->drain_save(); impl_->abandon_content(); impl_->foreground_job.reset();
    impl_->stop_covers(); impl_->clear_prefetch(); impl_->stop_counting_job();
    impl_->power_return_canvas.reset(); impl_->power_return_hits.clear();
    impl_->power_request = {};
    if (install) impl_->draw_installing();
    else impl_->draw_power_off(impl_->power_saved_note());
    impl_->overlay(true);
    return true;
}
bool ReaderApp::close_power_files() {
    if (impl_->display.busy() || impl_->view != View::PowerOff) return false;
    impl_->document.close();
    impl_->release_author_fonts();
    impl_->ui.open_fallback(); impl_->body.open_fallback();
    impl_->library_font.close(); impl_->library_font_path.clear();
    impl_->library_uses_body = false;
    impl_->identities.clear(); impl_->font_scan.reset(); impl_->font_identity.clear();
    return true;
}
void ReaderApp::show_power_off_help() {
    if (impl_->display.busy() || impl_->view != View::PowerOff) return;
    impl_->draw_power_off(impl_->trf("{saved}请长按电源键强制关机；若连接 USB，请拔下电源线。", {{"saved", impl_->power_saved_note()}}), true);
    impl_->overlay(true);
}
bool ReaderApp::begin_sleep() {
    if (impl_->display.busy() || impl_->power_menu_wait || impl_->usb_prepared ||
        impl_->view == View::PowerMenu || impl_->view == View::PowerOff || update_busy()) return false;
    impl_->pause_statistics();
    impl_->statistics_sleep_flushed = false;
    impl_->sleep_onboarding = impl_->view == View::Onboarding;
    impl_->sleep_origin = impl_->origin_for(impl_->view);
    impl_->sleep_destination_captured = true;
    impl_->show_sleep_wait();
    return true;
}
bool ReaderApp::sleep_pending() const {
    if (impl_->sleep_cover_deadline) return true;
    if (!impl_->sleep_destination_captured) return false;
    if (impl_->counting_job && impl_->counting_job->request.statistics) return true;
    return impl_->foreground_job && ContentJob::dictionary_kind(impl_->foreground_job->request.kind) &&
        impl_->foreground_job->started() && !impl_->foreground_job->done();
}
void ReaderApp::cancel_sleep() {
    impl_->statistics_sleep_flushed = false;
    impl_->sleep_destination_captured = false;
    impl_->custom_sleep_attempted = false;
    impl_->custom_prepare_finished = false; impl_->custom_prepare_attempts = 0;
    impl_->custom_sleep_image.reset();
    impl_->sleep_cover_image.reset();
    impl_->custom_sleep_deadline.reset();
    impl_->custom_rotation_name.clear();
    impl_->sleep_wait_restore_pending = bool(impl_->sleep_wait_footer);
    impl_->restore_sleep_wait();
    impl_->resume_statistics();
    if (impl_->sleep_cover_deadline) {
        impl_->sleep_cover_deadline.reset();
        if (impl_->counting_job) impl_->counting_job->cancel();
    }
    if (const auto target = std::exchange(impl_->sleep_wifi_target, std::nullopt);
        target && impl_->card_available && impl_->wifi_enabled() && !impl_->usb_prepared && !impl_->portal_live) {
        impl_->manual_hold = std::make_shared<std::atomic_bool>(false);
        impl_->wifi_worker = ++impl_->wifi_sequence;
        impl_->wifi_request = WifiOperation{WifiOperation::Kind::Connect, impl_->wifi_worker, *target, impl_->manual_hold};
    }
}
bool ReaderApp::prepare_sleep() {
    if (impl_->power_menu_wait) return false;
    impl_->pause_statistics();
    if (impl_->foreground_job && ContentJob::dictionary_kind(impl_->foreground_job->request.kind) &&
        impl_->foreground_job->started() && !impl_->foreground_job->done()) {
        impl_->foreground_job->cancel(); return false;
    }
    bool cover_attempted = false;
    if (impl_->sleep_cover_deadline) {
        impl_->poll_counting_job();
        if (impl_->counting_job) {
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
        const auto kind = impl_->counting_job->request.kind;
        impl_->sleep_waiting_for_cover = kind == ContentJob::Kind::SleepCover;
        if (kind != ContentJob::Kind::SleepCover && kind != ContentJob::Kind::LockScreen)
            impl_->counting_job->cancel();
        if (kind == ContentJob::Kind::LockScreen && impl_->settings.sleep_screen == "custom") {
            if (!impl_->custom_sleep_deadline)
                impl_->custom_sleep_deadline = std::chrono::steady_clock::now() + kLockScreenPrepareBudget;
            impl_->sleep_cover_deadline = impl_->custom_sleep_deadline;
        } else if (kind == ContentJob::Kind::SleepCover) {
            impl_->sleep_cover_deadline = std::chrono::steady_clock::now() + kLockScreenPrepareBudget;
        } else impl_->sleep_cover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        return false;
    }
    invalidate_identity_cache();
    if (!impl_->persist()) return false;
    if (impl_->has_book() && !impl_->session.visible_record()) return false;
    save_clock_sync();
    if (!impl_->statistics_sleep_flushed) {
        if (!impl_->flush_statistics_for_handoff()) return false;
        impl_->statistics_sleep_flushed = true;
    }
    impl_->remember_position();
    impl_->save_pages();
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    if (impl_->settings.sleep_screen == "custom" && !impl_->custom_sleep_attempted) {
        impl_->covers.clear(); impl_->make_room_for_cold_image();
        impl_->prepare_custom_sleep(true);
        if (impl_->counting_job) {
            impl_->sleep_waiting_for_cover = false;
            impl_->sleep_cover_deadline = impl_->custom_sleep_deadline;
            return false;
        }
    }
    const bool show_book = impl_->sleep_screen_shows_book();
    if (!cover_attempted && impl_->async_content && impl_->settings.sleep_screen == "cover" &&
        show_book && !txt_name(impl_->book_path)) {
        impl_->covers.clear(); impl_->make_room_for_cold_image();
        impl_->prepare_sleep_cover();
        if (impl_->counting_job) {
            impl_->sleep_waiting_for_cover = true;
            impl_->sleep_cover_deadline = std::chrono::steady_clock::now() + kLockScreenPrepareBudget;
            return false;
        }
    }
    const auto sync_state = show_book ? impl_->sync_badge() : std::string{};
    impl_->cancel_sync();
    network::release(impl_->manual_hold);
    network::release(impl_->wifi_hold);
    impl_->leave_join(Impl::WifiPage::Home);
    std::fill(impl_->account.password.begin(), impl_->account.password.end(), '\0');
    impl_->account.password.clear();
    impl_->release_author_fonts();
    impl_->draw_sleep(sync_state);
    if (impl_->view != View::Sleeping) return false;
    impl_->overlay(true);
    return true;
}
void ReaderApp::wake(bool slept) {
    if (impl_->display.busy() || (!slept && impl_->view == View::Error))
        return;
    impl_->wifi_status.phase = WifiPhase::Offline;
    impl_->wifi_status.manual = false;
    impl_->wifi_status.ssid.clear(); impl_->wifi_status.detail.clear();
    impl_->wifi_notice.clear();
    impl_->sleep_cover_image.reset();
    impl_->custom_sleep_attempted = false;
    impl_->custom_prepare_finished = false; impl_->custom_prepare_attempts = 0;
    impl_->custom_sleep_deadline.reset();
    impl_->sleep_destination_captured = false;
    impl_->custom_notice_pending = slept && impl_->settings.sleep_screen == "custom" && !impl_->custom_notice.empty();
    if (slept) impl_->sleep_wifi_target.reset();
    else cancel_sleep();
    if (impl_->sleep_onboarding) {
        impl_->draw_onboarding(); impl_->overlay(true);
    } else if (impl_->sleep_origin == Impl::Origin::Reading && impl_->has_book()) {
        if (impl_->async_content) { impl_->statistics_resume_open = true; impl_->open(impl_->book_path); }
        else impl_->recover_body();
    } else {
        impl_->return_to_origin(impl_->sleep_origin, true);
    }
}
void ReaderApp::card_unavailable(CardIssue issue) {
    impl_->pause_statistics();
    impl_->retire_statistics_mount();
    stop_wifi();
    impl_->power_return_canvas.reset(); impl_->power_request = {};
    invalidate_identity_cache();
    impl_->release_author_fonts();
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    impl_->card_issue = issue;
    if (impl_->usb_prepared) { show_usb_transfer(false, impl_->tr("TF 卡已移除。请退出 USB 传输，插回 TF 卡后重启。")); return; }
    impl_->cancel_sync();
    impl_->covers.clear();
    impl_->entries.clear();
    impl_->library_resume_omitted = false;
    const bool unsaved = impl_->card_available && impl_->session.needs_save();
    impl_->card_available = false;
    impl_->library_font.close();
    impl_->library_uses_body = false; impl_->library_font_path.clear();
    impl_->ui.open_fallback();
    impl_->body.open_fallback();
    impl_->effective_font.clear();
    if (impl_->view == View::Onboarding) {
        impl_->draw_onboarding(); impl_->overlay();
    } else if (issue != CardIssue::Removed || impl_->view == View::Library) {
        impl_->draw_library(); impl_->overlay();
    } else {
        impl_->error(unsaved ? impl_->tr("TF 卡已移除。最近的阅读位置没有保存。插回 TF 卡后，请重启设备。")
                             : impl_->tr("TF 卡已移除。插回 TF 卡后，请重启设备。"));
    }
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
    return (impl_->view == View::Statistics && (impl_->statistics_loading || impl_->statistics_redraw)) ||
           !!impl_->foreground_job || impl_->pending_content.has_value() ||
           (impl_->async_content && impl_->view == View::Reading && !impl_->image_viewing && impl_->body_images_pending);
}
bool ReaderApp::background_due() const {
    return (impl_->statistics_flush_requested && impl_->statistics_collector.pending_count() &&
            impl_->card_available && !impl_->usb_prepared && !impl_->portal_live && !impl_->statistics_serial_transfer &&
            !impl_->statistics_retry_ms && !impl_->session.needs_save() && !impl_->sleep_destination_captured &&
            impl_->view != View::PowerMenu && impl_->view != View::PowerOff && impl_->view != View::Sleeping) ||
           !!impl_->cover_job || impl_->pending_covers.has_value() || !!impl_->counting_job ||
           impl_->chapter_cache_due() || impl_->custom_prepare_due() || impl_->prefetch_due() ||
           impl_->chapter_prefetch_due() || impl_->counting_due();
}
#if !defined(ESP_PLATFORM) || defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
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
    if (cancel && impl_->counting_job && !impl_->counting_job->request.statistics) impl_->counting_job->cancel();
    impl_->poll_counting_job();
    if (impl_->sleep_wait_restore_pending) impl_->restore_sleep_wait();
    if (impl_->sleep_wait_footer && !impl_->sleep_wait_restore_pending && impl_->view == impl_->sleep_wait_view) return;
    if (!cancel || !impl_->dictionary_contact) impl_->flush_dictionary_selection();
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
    if (!cancel && impl_->statistics_waiting_control && !impl_->counting_job && !impl_->display.busy()) {
        const auto [origin, id] = *std::exchange(impl_->statistics_waiting_control, std::nullopt);
        if (impl_->view == origin && !impl_->sleep_destination_captured && !impl_->usb_prepared)
            impl_->control(id);
    }
    impl_->run_statistics(!cancel);
    if (!cancel) impl_->poll_dictionary_panel();
    if (!cancel) impl_->flush_entry();
    if (!cancel && impl_->custom_notice_pending &&
        !impl_->sleep_cover_deadline && !impl_->display.busy() && !impl_->content_loading &&
        !impl_->save_failed && !impl_->recent_save_failed &&
        (impl_->view == View::Library || impl_->view == View::Reading || impl_->view == View::Settings)) {
        impl_->custom_notice_pending = false;
#ifdef ESP_PLATFORM
        ESP_LOGW("reader_app", "Sleep image notice: %s", impl_->custom_notice.c_str());
#endif
        impl_->message = impl_->tr(kSleepFallbackNotice);
        if (impl_->view == View::Reading) impl_->footer();
        else if (impl_->view == View::Library) impl_->footer_bar(impl_->notice(), impl_->library_page, impl_->library_pages());
        else impl_->footer_bar(impl_->notice());
        const int top = impl_->view == View::Reading ? impl_->reading_status_top() : Impl::footer_zone;
        impl_->display.present({{"ui", 0, 0, ++impl_->ui_sequence}, {}, FramePurpose::Overlay,
            RefreshIntent::Content, RefreshRegion{0, top, 684, 1216 - top}});
    }
    if (!cancel && !impl_->pending_wifi_record.empty() && impl_->card_available &&
        !impl_->portal_live && !impl_->usb_prepared && !impl_->network_worker_id &&
        !impl_->display.busy() && !impl_->save_job && !impl_->bookmark_job && !impl_->foreground_job) {
        std::string error;
        const auto name = std::exchange(impl_->pending_wifi_record, std::string());
        if (!provision::save_connection(impl_->root, name, std::time(nullptr), error))
            impl_->wifi_notice = impl_->tr("连接记录未保存，请检查 TF 卡。");
    }
    if (impl_->power_menu_wait) {
        if (impl_->view != *impl_->power_menu_wait || !impl_->card_available) impl_->power_menu_wait.reset();
        else if (!cancel && !impl_->counting_job && !impl_->display.busy() && !impl_->save_job &&
                 !impl_->bookmark_job && !impl_->foreground_job && !impl_->content_loading) {
            impl_->power_menu_wait.reset();
            show_power_menu();
        }
    }
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
    if (stop && stop()) {
        if (impl_->counting_job && !impl_->counting_job->request.statistics) impl_->counting_job->cancel();
        return;
    }
    impl_->run_statistics(true);
    if (impl_->counting_job) return;
    if (impl_->chapter_cache_due()) impl_->write_chapter_cache();
    else if (impl_->custom_prepare_due()) impl_->prepare_custom_sleep(false);
    else if (impl_->prefetch_due()) impl_->prepare_next(stop);
    else if (impl_->chapter_prefetch_due()) impl_->prepare_chapter();
    else if (impl_->counting_due()) impl_->count_pages(stop);
}
void ReaderApp::show_error(const std::string &message) {
    if (!impl_->display.busy())
        impl_->error(impl_->tr(message.c_str()));
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
    if (!impl_->usb_prepared) impl_->network_complete(id, status, body, impl_->tr(error.c_str()), failure);
}
}
