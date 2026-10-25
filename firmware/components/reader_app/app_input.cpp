#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::error(const std::string &text_in, const std::string &retry) {
    reset_dictionary_contact();
    sleep_wait_footer.reset();
    sleep_wait_restore_pending = false;
    // Repainting an error must retain the destination captured when it first appeared.
    error_origin = origin_for(view);
    image_viewing = false;
    viewer_original.reset();
    image_contrast_level = 0;
    // The argument may alias message, which is assigned below.
    const std::string detail = text_in;
#ifdef ESP_PLATFORM
    ESP_LOGW("reader_app", "Error shown: %s", detail.c_str());
#endif
    abandon_content();
    if (drawing_error) return;
    drawing_error = true;
    // The page offers a save retry only for a failed commit, not for one still pending.
    if (card_available && session.needs_save() && !save_failed)
        persist();
    cancel_sync();
    link_return = confirmed_link_return;
    sync_return = confirmed_sync_return;
    message = detail;
    retry_path = retry;
    std::size_t split = std::string::npos, mark = 0;
    for (const char *separator : {"。", "，", "："}) {
        const auto found = detail.find(separator);
        if (found < split) { split = found; mark = std::strlen(separator); }
    }
    const auto title = detail.substr(0, split);
    const auto body = split == std::string::npos ? std::string() : detail.substr(split + mark);
    std::uint32_t visible_offset;
    const bool available = session.visible_record() && chapter.offset_for(session.visible_record()->anchor, visible_offset);
    const bool retained = (!async_content || available) && !foreground_job && card_available && has_book() && session.visible_record() &&
                          restore_confirmed_position() && panel_background(752, false);
    view = View::Error;
    hits.clear();
    const bool unsaved = session.needs_save();
    const bool resumes = error_origin == Origin::Reading && has_book() && (session.visible_record() || session.saved_record());
    if (retained) {
        canvas.fill_rect(0, 752, 684, 464, 15);
        canvas.line(0, 752, 683, 752, 1, 3);
        text(title, 36, 784, 32, 612, 1, true);
        title_lines(body, 36, 850, 612, 26, false, 3);
        note(error_origin == Origin::Reading ? "保留最后确认的位置。" :
             error_origin == Origin::Settings ? "返回将回到设置首页。" : "返回将回到书库。", 1012);
    } else {
        canvas.clear();
        header(retry.empty() ? "暂时无法继续" : "无法打开", unsaved || resumes ? 0 : Back,
               error_origin == Origin::Settings ? "设置" : "书库");
        title_lines(title, 36, 160, 612, 40, true);
        int y = 160 + std::min(wrapped(title, 0, 0, 612, 40, false), 96) + 28;
        if (!body.empty()) y += wrapped(body, 36, y, 612, 28) + 28;
        if (!retry.empty()) {
            y += wrapped(retry, 36, y, 612, 24, true, 3);
            wrapped("可重试一次；仍失败时请检查 TF 卡。", 36, y, 612, 24, true, 3);
        }
    }
    // Two actions share the row with the recommended one on the right; one action fills it.
    const auto leave = return_label(error_origin);
    const bool second = unsaved || !retry.empty();
    button(Back, leave, 24, 1080, second ? 306 : 636, 96, !second);
    if (unsaved) button(SaveRetry, "重试保存", 354, 1080, 306, 96, true);
    else if (!retry.empty()) button(Retry, "重试", 354, 1080, 306, 96, true);
    drawing_error = false;
    if (!display.busy()) overlay();
}


void ReaderApp::Impl::back() {
    if (view == View::Dictionary) {
        if (dictionary_choosing) cancel_dictionary_sources(); else close_dictionary();
        return;
    }
    if (view == View::DictionarySettings) { leave_dictionary_settings(); return; }
    if (view == View::NetworkClosing) return;
    if (network_worker_id && active_network) { wait_network(Back); return; }
    if (view == View::Typography && font_picker_open) { cancel_font_picker(); overlay(); return; }
    if (view == View::TextEntry) {
        const auto field = entry.field;
        entry = {};
        if (field == TextEntry::Field::WifiPassword && join_manual) {
            // The name stays so that the first step can be corrected.
            join_draft.password.clear();
            open_entry(TextEntry::Field::WifiName, join_draft.ssid);
            return;
        }
        if (field == TextEntry::Field::WifiPassword || field == TextEntry::Field::WifiName) {
            leave_join(join_origin);
            draw_wifi();
        } else draw_sync_settings();
        overlay();
        return;
    }
    if (view == View::Update) {
        if (update_page == UpdatePage::Notes) { update_page = notes_return; draw_update(); overlay(); return; }
        // The other pages end through their own buttons.
        if (update_page != UpdatePage::Home && update_page != UpdatePage::Failed) return;
        update_page = UpdatePage::Home; update_notice.clear();
        draw_settings(); overlay(); return;
    }
    if (view == View::WifiSettings) {
        if (portal_live) { draw_portal(); overlay(); return; }
        if (wifi_worker && wifi_kind == WifiOperation::Kind::Clock) network::release(wifi_hold);
        if (wifi_page == WifiPage::Joining || wifi_page == WifiPage::JoinFailed || wifi_page == WifiPage::Joined)
            return;  // These pages end through their own buttons.
        if (wifi_page != wifi_root) {
            if (wifi_page == WifiPage::Add) {
                // A scan in progress is abandoned; its late result carries a stale ID.
                network::release(wifi_hold);
                if (wifi_worker && wifi_kind == WifiOperation::Kind::Scan) { wifi_worker = 0; wifi_request.reset(); }
            }
            wifi_page = wifi_page == WifiPage::Detail ? WifiPage::Saved
                      : wifi_page == WifiPage::Forget ? WifiPage::Detail : WifiPage::Home;
            draw_wifi(); overlay(); return;
        }
        wifi_page = wifi_root = WifiPage::Home;
        if (std::exchange(wifi_from_sync, false)) { draw_sync(); overlay(); return; }
        if (std::exchange(wifi_from_transfer, false)) { draw_import(); overlay(); return; }
    }
    if (view == View::SyncSettings) {
        if (account_page == AccountPage::Remove) { account_page = AccountPage::Form; draw_sync_settings(); overlay(); return; }
        if (account_page == AccountPage::Verifying) { cancel_account(); draw_sync_settings(); overlay(); return; }
        std::fill(account.password.begin(), account.password.end(), '\0');
        account.password.clear();
        if (std::exchange(account_from_sync, false) && has_book()) {
            refresh_badge_config(); cancel_sync(); sync_status.clear(); draw_sync(); overlay(); return;
        }
    }
    if (view == View::LockScreens || view == View::LockScreenPreview) { draw_power(); overlay(); return; }
    if (view == View::PowerMenu) { close_power_menu(); return; }
    if (view == View::Portal) return;
    if (view == View::BookManagement) { draw_details(); overlay(); return; }
    if (usb_prepared) {
        if (usb_confirm) { usb_confirm = false; draw_usb(); overlay(); }
        return;
    }
    if (view == View::RefreshConfirm || view == View::RefreshInterval || view == View::PageEffects) {
        draw_refresh(); overlay(); return;
    }
    if (view == View::Help && !help_from_settings) { draw_library(); overlay(); return; }
    if (view == View::Help || view == View::About || view == View::WifiSettings || view == View::PowerSettings || view == View::StorageSettings || view == View::InputSettings ||
        view == View::SyncSettings || (view == View::Import && import_from_settings) ||
        (view == View::Refresh && refresh_from_settings)) {
        draw_settings(); overlay(); return;
    }
    if (view == View::Sync || view == View::Confirmation) {
        cancel_sync();
        if (has_book())
            recover_body();
        else {
            draw_library();
            overlay();
        }
        return;
    }
    if (view == View::Library) {
        if (!folder.empty()) {
            auto p = folder.find_last_of('/');
            const auto parent = p == std::string::npos ? "" : folder.substr(0, p);
            folder = folder_root || parent == library_home_root ? "" : parent;
            library_page = 0;
            scan_library();
            draw_library();
            overlay();
        }
        return;
    }
    if (view == View::Tools || view == View::Contents || view == View::Typography ||
        view == View::Refresh) {
        show_page(offset);
        return;
    }
    if (view == View::Error) {
        return_to_origin(error_origin);
        return;
    }
    draw_library();
    overlay();
}


void ReaderApp::Impl::wifi_control(int id) {
    const auto index = [&](int base) { return static_cast<std::size_t>(id - base); };
    std::string detail;
    if (id >= WifiNearbyBase) {
        if (view != View::WifiSettings || wifi_page != WifiPage::Add || index(WifiNearbyBase) >= wifi_nearby.size()) return;
        const auto& found = wifi_nearby[index(WifiNearbyBase)];
        join_draft = {found.ssid, ""};
        join_manual = false; join_open = found.open; join_secured = !found.open; join_origin = WifiPage::Add;
        if (join_open) join_wifi();
        else open_entry(TextEntry::Field::WifiPassword, "");
        return;
    }
    if (id >= WifiModeBase) {
        if (view != View::WifiSettings || wifi_page != WifiPage::Mode || index(WifiModeBase) >= wifi_saved.size()) return;
        wifi_notice = provision::save_selected_network(root, wifi_saved[index(WifiModeBase)].ssid, detail)
            ? "" : "连接方式未保存，请检查 TF 卡。";
        draw_wifi(); overlay(); return;
    }
    if (id >= WifiSavedBase) {
        if (view != View::WifiSettings || wifi_page != WifiPage::Saved || index(WifiSavedBase) >= wifi_saved.size()) return;
        wifi_choice = wifi_saved[index(WifiSavedBase)].ssid;
        wifi_page = WifiPage::Detail;
        draw_wifi(); overlay(); return;
    }
    switch (id) {
    case WifiConnect: connect_wifi(); return;
    case WifiClock:
        if (wifi_free(true)) {
            auto targets = wifi_targets(detail);
            if (targets.networks.empty()) wifi_notice = detail;
            else start_wifi(WifiOperation::Kind::Clock, std::move(targets), wifi_hold);
        }
        break;
    case WifiClockCancel:
        network::release(wifi_hold);
        wifi_notice = "正在结束校准…";
        break;
    case WifiDisconnect:
        if (wifi_worker && wifi_kind == WifiOperation::Kind::Clock) network::release(wifi_hold);
        network::release(manual_hold); cancel_sync();
        if (wifi_status.phase != WifiPhase::Offline) { wifi_status.phase = WifiPhase::Closing; wifi_notice = "正在结束…"; }
        else wifi_notice.clear();
        break;
    case WifiManage: wifi_page = WifiPage::Saved; wifi_notice.clear(); break;
    case WifiModeOpen: wifi_page = WifiPage::Mode; wifi_notice.clear(); break;
    case WifiModeAuto:
        // Without a pinned name the file already selects automatic connection.
        wifi_notice = wifi_target.empty() || provision::save_selected_network(root, "", detail)
            ? "" : "连接方式未保存，请检查 TF 卡。";
        break;
    case WifiAdd:
    case WifiRescan: scan_wifi(); return;
    case WifiManualName:
        network::release(wifi_hold);
        if (wifi_worker && wifi_kind == WifiOperation::Kind::Scan) { wifi_worker = 0; wifi_request.reset(); }
        join_draft = {};
        join_manual = true; join_open = join_secured = false; join_origin = WifiPage::Add;
        open_entry(TextEntry::Field::WifiName, "");
        return;
    case WifiUpdatePassword:
        if (!wifi_free()) break;
        join_draft = {wifi_choice, ""};
        join_manual = false; join_open = join_secured = false; join_origin = WifiPage::Detail;
        open_entry(TextEntry::Field::WifiPassword, "");
        return;
    case WifiForget: wifi_page = WifiPage::Forget; break;
    case WifiForgetConfirm:
        wifi_page = WifiPage::Detail;
        if (wifi_worker || network::live(manual_hold) || wifi_online()) wifi_notice = "请先断开 Wi-Fi，再忘记网络。";
        else if (!provision::save_network_edit(root, {wifi_choice, ""}, true, 0, detail)) wifi_notice = "忘记网络失败，请检查 TF 卡。";
        else {
            wifi_notice = wifi_choice == wifi_target ? "网络已忘记，连接方式已改回自动。" : "网络已忘记。";
            wifi_choice.clear();
            wifi_page = WifiPage::Saved;
        }
        break;
    case WifiJoinCancel:
        // The worker stops at its next cancellation check; its result carries a stale ID.
        network::release(wifi_hold);
        wifi_worker = 0; wifi_request.reset();
        // Every flow with a password page returns to it with its draft, also when the password is empty.
        if (!join_open) { open_entry(TextEntry::Field::WifiPassword, join_draft.password); return; }
        leave_join(join_origin);
        break;
    case WifiJoinRetry: join_wifi(); return;
    case WifiJoinEdit: open_entry(TextEntry::Field::WifiPassword, join_draft.password); return;
    case WifiJoinList: leave_join(join_origin == WifiPage::Detail ? WifiPage::Saved : WifiPage::Add); break;
    case WifiJoinDone: leave_join(WifiPage::Home); break;
    case SyncEnable: {
        if (!begin_settings_edit(true)) return;
        auto values = edit_snapshot.global_settings; values.wifi_enabled = true;
        if (store.apply_settings(edit_snapshot, values, false, detail) != StoreStatus::Ok) {
            sync_status = "Wi-Fi 开关未保存，请检查 TF 卡。"; draw_sync(); overlay(); return;
        }
        settings.wifi_enabled = true; check_sync(); return;
    }
    case SyncOnceAuto: check_sync(true); return;
    case SyncStay: {
        // The reading header keeps its reminder until the next check or another book.
        const auto reminder = remote_anchor;
        cancel_sync();
        remote_anchor = reminder;
        if (has_book()) recover_body();
        return;
    }
    case SyncAccount:
        cancel_sync();
        account_from_sync = true;
        open_account();
        draw_sync_settings(); overlay(); return;
    case AccountEndpoint: open_entry(TextEntry::Field::Endpoint, account.endpoint); return;
    case AccountUser: open_entry(TextEntry::Field::Username, account.username); return;
    case AccountPassword: open_entry(TextEntry::Field::Password, account.password); return;
    case AccountVerify: verify_account(); return;
    case AccountCancel: cancel_account(); draw_sync_settings(); overlay(); return;
    case AccountRemove: account_page = AccountPage::Remove; draw_sync_settings(); overlay(); return;
    case AccountRemoveConfirm: {
        if (!bookmark_io_ready()) return;
        const auto removed = store.remove_private("sync.toml");
        cancel_sync();
        open_account();
        refresh_badge_config();
        account_notice = removed == StoreStatus::Ok || removed == StoreStatus::Missing ? "同步账户已移除。" : "同步账户未移除，请检查 TF 卡。";
        draw_sync_settings(); overlay(); return;
    }
    case EntryClear: entry.value.clear(); entry.field_dirty = true; flush_entry(); return;
    case EntryMask: entry.masked = !entry.masked; entry.field_dirty = true; flush_entry(); return;
    case EntryDefault: entry.value = provision::default_endpoint; entry.field_dirty = true; flush_entry(); return;
    default: return;
    }
    draw_wifi(); overlay();
}


void ReaderApp::Impl::control(int id) {
    if (id == DictionaryOpen) { open_dictionary_settings(); return; }
    if (view == View::Typography && font_picker_open && id != FullRefresh) {
        font_picker_control(id); return;
    }
    if (view == View::Update && update_control(id)) return;
    if (id == UpdateOpen) { update_open(); return; }
    if (id == SleepCustom) { draft.sleep_screen = "custom"; draw_power(); overlay(); return; }
    if (id == SleepOverlay) { draft.sleep_overlay = !draft.sleep_overlay; draw_power(); overlay(); return; }
    if (id == SleepPreview) { preview_lock_screen(); return; }
    if (id == SleepChoose || id == SleepRefresh) { list_lock_screens(); return; }
    if (id == SleepRotation) {
        draft.sleep_image.clear(); draft.sleep_screen = "custom"; draw_lock_screens(); overlay(); return;
    }
    if (view == View::LockScreens && id >= LockScreenBase && id < LockScreenBase + 256) {
        const auto index = static_cast<std::size_t>(id - LockScreenBase);
        if (index < lock_screens.size()) {
            draft.sleep_image = lock_screens[index]; draft.sleep_screen = "custom";
            draw_lock_screens(); overlay();
        }
        return;
    }
    if (view == View::NetworkClosing) {
        if (id == ContinueOffline) {
            network_after_cancel = 0;
            return_to_origin(origin_for(view));
        }
        return;
    }
    if (network_worker_id && active_network && id != FullRefresh) { wait_network(id); return; }
    if (view == View::Sync && (id == Contents || id == Typography || id == Refresh)) cancel_sync();
    if (usb_prepared && id != UsbExit && id != UsbExitConfirm && id != UsbCancel)
        return;
    if (id == WifiSettings || (id == WifiModeOpen && view == View::Sync)) {
        wifi_from_sync = view == View::Sync;
        // The network page reached from the library stays on the way back to the library.
        wifi_from_transfer = view == View::Import && !import_from_settings;
        if (!portal_live) cancel_sync();
        // The sync panel's network row opens the connection mode directly.
        wifi_page = wifi_root = id == WifiModeOpen ? WifiPage::Mode : WifiPage::Home;
        wifi_choice.clear();
        draw_wifi(); overlay(); return;
    }
    if (portal_live && (view == View::Portal || view == View::WifiSettings) && id != Back && id != FullRefresh) {
        if (id == PortalStop && !portal_closing) {
            portal_open_wifi = view == View::WifiSettings;
            portal_action = PortalAction::Stop;
            portal_closing = true;
            draw_portal(); overlay();
        }
        return;
    }
    if ((id == WifiUpload || id == UsbStart) && view == View::Library) import_from_settings = false;
    if (id == WifiUpload || id == WifiUploadStation) {
        if (!wifi_enabled()) {
            // The network page states that Wi-Fi is off and leads back to the library.
            if (view == View::Library) { draw_import(); overlay(); return; }
            wifi_page = wifi_root = WifiPage::Home; draw_wifi(); overlay(); return;
        }
        ++portal_generation;
        portal_requested = true;
        portal_request_view = view;
        portal_action = id == WifiUpload ? PortalAction::Upload : PortalAction::UploadStation;
        return;
    }
    if (id == UsbStart) {
        usb_request_view = view;
        usb_action = UsbTransferAction::Start;
        return;
    }
    if (id == UsbExit || id == UsbCancel) {
        usb_confirm = id == UsbExit;
        draw_usb(); overlay(); return;
    }
    if (id == UsbExitConfirm) {
        usb_action = UsbTransferAction::Exit;
        return;
    }
    if (id >= WifiConnect) { wifi_control(id); return; }
    if (id >= BookmarkBase) {
        const auto index = static_cast<std::size_t>(id - BookmarkBase);
        if (view != View::Contents || !bookmarks_usable() || index >= bookmarks.bookmarks.size()) return;
        const auto target = bookmarks.bookmarks[index].anchor;
        if (async_content) { clear_page_history(); begin_seek(target); return; }
        std::uint32_t position;
        if (!seek(target, position, "书签位置无法恢复，原书内容可能已更改")) return;
        clear_page_history();
        show_page(position);
        return;
    }
    if (id >= DetailBase) {
        const auto index = static_cast<std::size_t>(id - DetailBase);
        if (index < entries.size() && !entries[index].directory) {
            inspect_book(entries[index].path); if (!display.busy()) overlay();
        }
        return;
    }
    if (id >= TocToggleBase && id < LinkBase) {
        const auto index = static_cast<std::size_t>(id - TocToggleBase);
        if (index < document.toc().size() && toc_parent(index)) {
            if (!toc_collapsed.erase(index)) toc_collapsed.insert(index);
            draw_contents(); overlay();
        }
        return;
    }
    if (id >= LinkBase) {
        const auto i = static_cast<std::size_t>(id - LinkBase);
        if (i >= chapter.links.size())
            return;
        const auto link = chapter.links[i];
        if (link.external) {
            message = "外部链接无法在阅读器中打开";
            draw_tools();
            overlay();
            return;
        }
        std::size_t index;
        if (!document.chapter_index_for(link.resource, index)) {
            error("链接目标不在正文中");
            return;
        }
        const auto origin = session.visible_record();
        if (!origin)
            return;
        if (async_content) {
            if (!link_return) link_return = origin->anchor;
            clear_page_history();
            begin_chapter(index, 0, ContentJob::Target::Fragment, 0, {}, link.fragment);
            return;
        }
        std::uint32_t target = 0;
        if (!seek_fragment(index, link.fragment, target, "链接锚点不存在"))
            return;
        if (!link_return)
            link_return = origin->anchor;
        clear_page_history();
        show_page(target);
        return;
    }
    if (id >= TocBase) {
        const auto index = static_cast<std::size_t>(id - TocBase);
        if (index >= document.toc().size())
            return;
        const auto item = document.toc()[index];
        std::size_t target;
        if (!document.chapter_index_for(item.resource, target)) {
            error("目录目标不在正文中");
            return;
        }
        if (async_content) {
            clear_page_history();
            begin_chapter(target, 0, ContentJob::Target::Fragment, 0, {}, item.fragment);
            return;
        }
        std::uint32_t position = 0;
        if (!seek_fragment(target, item.fragment, position, "目录锚点不存在"))
            return;
        show_page(position);
        return;
    }
    if (id >= EntryBase) {
        const auto i = static_cast<std::size_t>(id - EntryBase);
        if (i >= entries.size())
            return;
        const auto entry = entries[i];
        if (entry.directory) {
            folder = entry.path;
            library_page = 0;
            scan_library();
            draw_library();
            overlay();
        } else if (async_content) open(entry.path);
        else
            defer("正在打开书籍…", [this, path = entry.path] { open(path); });
        return;
    }
    switch (id) {
    case Help:
        help_from_settings = view == View::Settings;
        message.clear(); help_page = 0; draw_help();
        break;
    case About:
        message.clear(); draw_about();
        break;
    case LinkReturn: {
        if (!link_return)
            return;
        const auto target = *link_return;
        if (async_content) { link_return.reset(); sync_return = false; clear_page_history(); begin_seek(target); return; }
        std::uint32_t target_offset;
        if (!seek(target, target_offset, "原位置无法恢复"))
            return;
        link_return.reset();
        sync_return = false;
        clear_page_history();
        show_page(target_offset);
        return;
    }
    case PowerMenu:
        if (!display.busy() && !active_network && !update_waiting() && !content_loading && !foreground_job && !usb_prepared && view != View::Portal) {
            if (counting_job && (counting_job->request.kind == ContentJob::Kind::SleepCover || counting_job->request.kind == ContentJob::Kind::LockScreen)) {
                counting_job->cancel();
                power_menu_wait = view;
                return;
            }
            drain_save(); stop_covers(); clear_prefetch(); stop_counting_job();
            power_return_canvas = canvas; power_return_hits = hits; power_return_view = view;
            power_return_message = message; power_selected = PowerAction::None; power_save_failed = false;
            draw_power_menu(); overlay();
        }
        return;
    case PowerShutdown:
    case PowerRestart:
        if (view == View::PowerMenu && !power_save_failed) {
            power_selected = id == PowerRestart ? PowerAction::Restart : PowerAction::Shutdown;
            power_request = {power_selected, false};
        }
        return;
    case PowerForce:
        if (view == View::PowerMenu && power_save_failed)
            power_request = {power_selected, true};
        return;
    case PowerCancel:
        close_power_menu(); return;
    case BookmarkToggle:
        toggle_bookmark(); return;
    case ContentsToc:
    case ContentsBookmarks:
        if (view != View::Contents) return;
        contents_bookmarks = id == ContentsBookmarks;
        if (contents_bookmarks) {
            // Opens on the screen holding the first bookmark at or after the visible page.
            const auto present = page_bookmarks();
            bookmark_page = present.empty() ? 0 : present.front() / bookmarks_per_screen;
        } else toc_locate_current = true;
        draw_contents(); overlay(); return;
    case Back:
        back();
        return;
    case Dismiss:
        if (view == View::Tools)
            show_page(offset);
        else {
            draw_library();
            overlay();
        }
        return;
    case Retry:
        if (!retry_path.empty()) open(retry_path);
        return;
    case SyncCancel:
        sync_cancelled(uploading);
        draw_sync();
        break;
    case Library:
        if (!persist()) {
            error(message);
            return;
        }
        remember_position();
        save_pages();
        scan_library();
        draw_library();
        overlay(true);
        return;
    case Tools:
        if (has_book()) {
            if (session.visible_record() || session.saved_record())
                recover_body();
            else
                open(book_path);
        } else if (!last_book.empty()) {
            if (async_content) open(last_book);
            else defer("正在打开书籍…", [this] { open(last_book); });
        }
        return;
    case Contents: {
        load_fonts();
        toc_page = 0;
        toc_collapsed.clear();
        const auto current = current_toc();
        const auto &toc = document.toc();
        for (std::size_t i = 0; i < toc.size(); ++i) {
            if (!toc_parent(i)) continue;
            auto end = i + 1;
            while (end < toc.size() && toc[end].depth > toc[i].depth) ++end;
            if (current <= i || current >= end) toc_collapsed.insert(i);
        }
        toc_locate_current = true;
        draw_contents();
        break;
    }
    case Typography:
        typography_from_settings = origin_for(view) == Origin::Settings;
        if (!begin_settings_edit())
            return;
        draft = edit_snapshot.config.settings;
        font_picker_open = false;
        parameter = FontSize;
        reset_overrides = false;
        book_scope = true;
        draw_typography();
        break;
    case Refresh:
        refresh_from_settings = view == View::Settings;
        if (!begin_settings_edit(refresh_from_settings))
            return;
        draw_refresh();
        break;
    case LibraryFolders: {
        if (!bookmark_io_ready()) return;
        const auto snapshot = store.settings_snapshot();
        auto values = snapshot.config.settings;
        values.library_folders = values.library_folders == "first" ? "recent" : "first";
        std::string detail;
        if (store.apply_settings(snapshot, values, false, detail) == StoreStatus::Ok) {
            library_page = 0;
            message = "文件夹排序已保存";
            scan_library();
        } else message = "文件夹排序未保存，请检查 TF 卡或设置文件";
        draw_input_settings(); break;
    }
    case LibrarySort: {
        if (!bookmark_io_ready()) return;
        const auto snapshot = store.settings_snapshot();
        auto values = snapshot.config.settings;
        values.library_sort = library_sort == "recent" ? "name" : library_sort == "name" ? "name_desc" : "recent";
        std::string detail;
        if (store.apply_settings(snapshot, values, false, detail) == StoreStatus::Ok) {
            library_sort = values.library_sort; settings.library_sort = library_sort;
            library_page = 0;
            message = library_sort == "recent" ? "按最近阅读排序" : library_sort == "name" ? "按书名升序排列" : "按书名降序排列";
            scan_library();
        } else message = "排序未保存，请检查 TF 卡或设置文件";
        draw_library(); break;
    }
    case LibraryList:
    case LibraryGrid:
        library_grid = id == LibraryGrid;
        library_page = 0;
        draw_library();
        break;
    case DetailCache:
        covers.erase(std::remove_if(covers.begin(), covers.end(), [&](const auto& cover) { return cover.path == detail_path; }), covers.end());
        if (detail_path == book_path) clear_page_history();
        message = "本书内存缓存已清理，阅读记录保留";
        draw_details();
        break;
    case DetailReset:
    case DetailDelete:
        confirm_management(id == DetailDelete);
        if (view == View::BookManagement) overlay();
        return;
    case ManageConfirm:
        manage_book();
        return;
    case DetailOpen:
        if (async_content) { open(detail_path); return; }
        defer("正在打开书籍…", [this] { open(detail_path); });
        return;
    case PageEffects:
        draw_page_effects();
        break;
    case EffectOff: case EffectRipple:
        page_effect = id == EffectOff ? PageEffect::Off : PageEffect::Ripple;
        draw_page_effects();
        break;
    case RefreshAccept:
        apply_refresh(RefreshFast);
        return;
    case RefreshInterval:
        draft = edit_snapshot.config.settings;
        draw_interval();
        break;
    case IntervalLess:
    case IntervalMore: {
        auto &interval = draft.refresh_preset == "fast" ? draft.fast_full_refresh_pages : draft.balanced_full_refresh_pages;
        interval = std::clamp(interval + (id == IntervalMore ? 1 : -1), 1, 30);
        draw_interval();
        break;
    }
    case IntervalSave:
        save_refresh(draft);
        return;
    case PowerSettings:
        if (!begin_settings_edit(true)) return;
        draft = edit_snapshot.config.settings;
        draw_power();
        break;
    case SleepCover:
    case SleepReading:
    case SleepSimple:
        draft.sleep_screen = id == SleepCover ? "cover" : id == SleepReading ? "reading" : "simple";
        draw_power();
        break;
    case SleepLess:
    case SleepMore:
        draft.auto_sleep_minutes = std::clamp(draft.auto_sleep_minutes + (id == SleepMore ? 5 : -5), 0, 240);
        draw_power();
        break;
    case PowerSave: {
        if (!bookmark_io_ready()) return;
        std::string detail;
        if (store.apply_settings(edit_snapshot, draft, false, detail) != StoreStatus::Ok) {
            error("休眠设置未保存。" + reason(detail) + "。"); return;
        }
        settings.auto_sleep_minutes = draft.auto_sleep_minutes;
        settings.sleep_screen = draft.sleep_screen;
        settings.sleep_image = draft.sleep_image;
        settings.sleep_overlay = draft.sleep_overlay;
        custom_prepare_finished = custom_sleep_attempted = false;
        custom_notice.clear(); custom_notice_pending = false;
        custom_sleep_image.reset();
        message = "休眠设置已保存";
        draw_settings();
        break;
    }
    case StorageSettings:
        message.clear(); draw_storage();
        break;
    case ClearCache:
        if (!bookmark_io_ready()) return;
        if (cancel_custom_preparation()) {
            clear_cache_pending = true;
            message = "正在停止图片准备，完成后清理缓存";
            draw_storage();
            break;
        }
        clear_cache_pending = false;
        abandon_content(); foreground_job.reset(); stop_covers(); clear_prefetch(); stop_counting_job();
        covers.clear(); custom_prepare_finished = false; custom_sleep_image.reset();
        message = store.clear_cache() == StoreStatus::Ok ? "缓存已清理，原书与阅读记录保留" : "缓存未完全清理，请检查 TF 卡";
        draw_storage();
        break;
    case InputSettings:
        message.clear();
        draw_input_settings();
        break;
    case SyncSettings:
        if (view == View::WifiSettings) leave_join(WifiPage::Home);
        account_from_sync = false;
        open_account();
        draw_sync_settings();
        break;
    case LibraryView: {
        if (!begin_settings_edit(true)) return;
        auto values = edit_snapshot.config.settings;
        const std::string previous = values.library_view == "grid" ? "卡片" : "列表";
        values.library_view = values.library_view == "grid" ? "list" : "grid";
        if (!bookmark_io_ready()) return;
        std::string detail;
        if (store.apply_settings(edit_snapshot, values, false, detail) == StoreStatus::Ok) {
            settings.library_view = values.library_view;
            library_grid = values.library_view == "grid";
            library_page = 0;
            message.clear();
        } else
            message = "“书库默认视图”未保存，仍按原设置（" + previous + "）生效。请检查存储卡。";
        draw_input_settings();
        break;
    }
    case WifiToggle: {
        if (!begin_settings_edit(true)) return;
        auto values = edit_snapshot.global_settings;
        values.wifi_enabled = !values.wifi_enabled;
        std::string detail;
        if (store.apply_settings(edit_snapshot, values, false, detail) != StoreStatus::Ok)
            wifi_notice = "Wi-Fi 开关未保存，仍按原设置生效。请检查存储卡。";
        else {
            settings.wifi_enabled = values.wifi_enabled;
            if (!values.wifi_enabled) {
                cancel_sync(); network::release(manual_hold); network::release(wifi_hold);
                if (wifi_status.phase != WifiPhase::Offline) wifi_status.phase = WifiPhase::Closing;
            }
            wifi_notice.clear();
        }
        draw_wifi();
        break;
    }
    case Settings:
        draw_settings();
        break;
    case Import:
        import_from_settings = view == View::Settings;
        draw_import();
        break;
    case Sync:
        if (!bookmark_io_ready()) return;
        refresh_badge_config();
        cancel_sync();
        sync_status.clear();
        draw_sync();
        break;
    case SyncCheck:
        check_sync();
        return;
    case SyncUpload:
        if (!network::live(sync_hold)) { check_sync(); return; }
        confirm_upload();
        overlay();
        return;
    case SyncConfirm:
        if (!network::live(sync_hold)) { check_sync(); return; }
        upload();
        return;
    case SyncJump: {
        if (!remote_anchor)
            return;
        sync::Config current;
        std::string detail;
        if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
            cancel_sync();
            sync_state = SyncState::ConfigChanged;
            draw_sync();
            overlay();
            return;
        }
        const auto target = *remote_anchor;
        if (async_content) {
            const auto origin = session.visible_record();
            cancel_sync();
            if (origin) { link_return = origin->anchor; sync_return = true; }
            clear_page_history(); begin_seek(target); return;
        }
        std::uint32_t start;
        if (!seek(target, start, "远端位置无法恢复"))
            return;
        const auto origin = session.visible_record();
        cancel_sync();
        if (origin) { link_return = origin->anchor; sync_return = true; }
        clear_page_history();
        show_page(start);
        return;
    }
    case ResumeDetails:
        inspect_book(has_book() ? book_path : last_book); if (!display.busy()) overlay(); return;
    case HelpPrevious: case HelpTools: case HelpNext: case HelpPosition: case HelpDevice:
        help_zone = id;
        if (id == HelpPosition) help_position = (help_position + 1) % 5;
        if (id == HelpDevice) help_device = (help_device + 1) % 3;
        draw_help(); break;
    case HelpKey1: case HelpKey2: case HelpKey3:
        help_key = id; draw_help(); break;
    case FullRefresh:
        force_refresh();
        return;
    case Prev:
    case Next:
        if (view == View::LockScreens) {
            if (id == Prev && lock_screen_page) --lock_screen_page;
            else if (id == Next) ++lock_screen_page;
            draw_lock_screens();
        } else if (view == View::Help) {
            if (id == Prev && help_page) --help_page;
            else if (id == Next && help_page + 1 < kHelpPages.size()) ++help_page;
            else return;
            draw_help();
        } else if (view == View::Library) {
            if (id == Prev && library_page)
                --library_page;
            else if (id == Next && library_page + 1 < library_pages())
                ++library_page;
            else return;
            draw_library();
        } else if (view == View::WifiSettings) {
            if (wifi_page != WifiPage::Add) return;
            if (id == Prev && wifi_add_page) --wifi_add_page;
            else if (id == Next) ++wifi_add_page;  // draw_wifi clamps to the last screen.
            draw_wifi();
        } else if (view == View::Contents && contents_bookmarks) {
            if (id == Prev && bookmark_page) --bookmark_page;
            else if (id == Next) ++bookmark_page;  // draw_bookmarks clamps to the last screen.
            draw_contents();
        } else if (view == View::Contents) {
            if (id == Prev && toc_page)
                --toc_page;
            else if (id == Next && toc_page + 1 < toc_starts.size())
                ++toc_page;
            draw_contents();
        } else {
            turn(id == Next);
            return;
        }
        break;
    case FontChoice:
        font_entry = draft.font;
        font_entry_weight = draft.font_weight;
        font_entry_reset = reset_overrides;
        {
            auto at = std::find(fonts.begin(), fonts.end(), draft.font);
            if (at == fonts.end()) {
                // One extra entry keeps a missing configured font visible after the scan's 256 files.
                if (fonts.size() < 257) fonts.push_back(draft.font);
                else fonts.back() = draft.font;
                at = fonts.end() - 1;
            }
            font_picker_page = static_cast<std::size_t>(at - fonts.begin()) / fonts_per_screen;
        }
        font_picker_open = true;
        draw_typography();
        break;
    case FontWeight:
    case FontSize:
    case LineHeight:
    case Margin:
    case Gap:
    case Indent:
        parameter = id;
        draw_typography();
        break;
    case Scope:
        book_scope = false;
        apply_typography();
        return;
    case Defaults:
        draft = edit_snapshot.global_settings;
        reset_overrides = true;
        draw_typography();
        break;
    case Minus:
    case Plus: {
        reset_overrides = false;
        const int direction = id == Plus ? 1 : -1;
        if (parameter == FontWeight) {
            const std::array<std::string, 4> weights{{"light", "regular", "medium", "bold"}};
            const auto at = std::find(weights.begin(), weights.end(), draft.font_weight);
            const int first = light_available(draft) ? 0 : 1;
            draft.font_weight = weights[std::clamp(static_cast<int>(at - weights.begin()) + direction, first, 3)];
        } else if (parameter == Gap)
            draft.paragraph_gap_lines =
                std::clamp(draft.paragraph_gap_lines + direction * .2, 0., 1.);
        else if (parameter == Indent)
            draft.indent_chars = std::clamp(draft.indent_chars + direction, 0, 2);
        else if (parameter == FontSize)
            draft.font_size_px = std::clamp(draft.font_size_px + direction * 2, 28, 56);
        else if (parameter == LineHeight)
            draft.line_height = std::clamp(draft.line_height + direction * .05, 1.35, 1.65);
        else
            draft.margin_x_px = std::clamp(draft.margin_x_px + direction * 4, 24, 52);
        if (draft.font_weight == "light" && !light_available(draft)) draft.font_weight = "regular";
        draw_typography();
        break;
    }
    case Apply:
        if (!typography_changed()) return;
        apply_typography();
        return;
    case RefreshClear:
    case RefreshBalanced:
    case RefreshFast:
        if (id == RefreshFast && edit_snapshot.config.settings.refresh_preset != "fast") {
            draw_refresh_confirm(); overlay(); return;
        }
        apply_refresh(id);
        return;
    case SaveRetry: {
        const auto origin = origin_for(view);
        if (!persist()) { error(message); return; }
        message = "进度已保存";
        return_to_origin(origin);
        return;
    }
    default:
        return;
    }
    overlay();
}


void ReaderApp::Impl::input(const AppInput &input) {
    const bool accepted = router.accept(input.event);
    if (dictionary_contact_input(input, accepted)) return;
    // Key taps change the entry model only, so they are kept while a frame is in flight.
    if (accepted && view == View::TextEntry && key_input(input)) return;
    if (!accepted || display.busy() || view == View::Sleeping || view == View::PowerOff)
        return;
    if (view == View::Dictionary || view == View::DictionarySettings) {
        if (input.queued) return;
        if (!input.touch) {
            if (input.event.input == Input::K2) force_refresh();
            else if (input.event.input == Input::K1 || input.event.input == Input::Previous) dictionary_control(Prev);
            else if (input.event.input == Input::K3 || input.event.input == Input::Next) dictionary_control(Next);
        } else for (const auto& hit : hits) if (hit.contains(input.x, input.y)) { dictionary_control(hit.id); break; }
        return;
    }
    if (async_content) {
        if (counting_job) counting_job->cancel();
    } else release_counting();
    if (content_loading) {
        if (!input.touch && input.event.input == Input::K2) force_refresh();
        else if (!input.queued && input.touch && loading_cancel.contains(input.x, input.y)) {
            const auto& request = pending_content ? *pending_content : foreground_job->request;
            const auto kind = request.kind;
            const bool preview = request.preview;
            const bool mapping = kind == ContentJob::Kind::MapUpload || kind == ContentJob::Kind::MapDownload;
            abandon_content();
            if (kind == ContentJob::Kind::ImageViewer) { close_image_viewer(); return; }
            link_return = confirmed_link_return;
            sync_return = confirmed_sync_return;
            if (kind == ContentJob::Kind::Chapter && sync_return) { recover_body(); return; }
            if (kind == ContentJob::Kind::LockScreenList || kind == ContentJob::Kind::LockScreenPreview) draw_power();
            else if (mapping) { sync_cancelled(false); draw_sync(); }
            else if (preview) draw_tools();
            else draw_library();
            overlay();
        }
        return;
    }
    if (image_viewing && view == View::Reading) {
        if (!input.queued && !input.touch && input.event.input == Input::K2) force_refresh();
        else if (!input.queued && input.touch && input.y < viewer_toolbar_height) {
            if (input.x >= viewer_action_left) {
                image_contrast_level = (image_contrast_level + 1) % (viewer_contrast_levels + 1);
                draw_image_viewer(true);
            } else close_image_viewer();
        }
        return;
    }
    const bool preserve_prefetch = view == View::Reading &&
        ((!input.touch && (input.event.input == Input::K3 || input.event.input == Input::Next || input.event.input == Input::K2)) ||
         (input.touch && input.x >= 479 && input.y < footer_top));
    if (!preserve_prefetch) clear_prefetch();
    if (input.queued) {
        const bool turn_touch = input.touch && input.y < footer_top && (input.x < 205 || input.x >= 479);
        const auto action = input.touch ? Action::None : map_input(context(), input.event.input);
        const bool turn_key = action == Action::PreviousPage || action == Action::NextPage;
        if (view != View::Reading || !turned || (!turn_touch && !turn_key))
            return;
        // Links are not followed: the contact was aimed at the page that has been replaced.
        turn(turn_touch ? input.x >= 479 : action == Action::NextPage);
        return;
    }
    turned = false;
    // A notice lasts until the input after the one that showed it; K2 only repaints the page.
    if (view != View::Import && view != View::WifiSettings && view != View::Portal &&
        (input.touch || input.event.input != Input::K2) && std::exchange(notice_shown, false)) message.clear();
    if (input.touch) {
        if (view == View::Reading) {
            if (input.y >= footer_top) {
                const bool device = input.x >= reading_footer::device_left;
                if (input.touch_bounds &&
                    (input.touch_bounds->min_y < footer_top ||
                     (input.touch_bounds->min_x >= reading_footer::device_left) != device ||
                     (input.touch_bounds->max_x >= reading_footer::device_left) != device)) return;
                if (device) {
                    switch (footer_device) {
                    case reading_footer::Device::Battery: footer_device = reading_footer::Device::Clock; break;
                    case reading_footer::Device::Clock: footer_device = reading_footer::Device::Hidden; break;
                    case reading_footer::Device::Hidden: footer_device = reading_footer::Device::Battery; break;
                    }
                } else {
                    switch (footer_position) {
                    case reading_footer::Position::Pages: footer_position = reading_footer::Position::Percent; break;
                    case reading_footer::Position::Chapters: footer_position = reading_footer::Position::ChapterName; break;
                    case reading_footer::Position::ChapterName: footer_position = reading_footer::Position::Hidden; break;
                    case reading_footer::Position::Hidden: footer_position = reading_footer::Position::Pages; break;
                    case reading_footer::Position::Percent: footer_position = reading_footer::Position::Chapters; break;
                    }
                }
                footer();
                display.present({{"ui", 0, 0, ++ui_sequence}, {}, FramePurpose::Overlay,
                                 RefreshIntent::Content, RefreshRegion{0, footer_top, 684, 1216 - footer_top}});
                return;
            }
            if (input.long_press && !image_at(input.x, input.y) && graphics::text_hit(page, input.x, input.y)) {
                begin_dictionary(input.x, input.y); return;
            }
            for (const auto &link : links)
                if (link.contains(input.x, input.y)) {
                    control(link.id);
                    return;
                }
            if (input.long_press)
                if (const auto* box = image_at(input.x, input.y)) {
                    begin_image_viewer(*box);
                    return;
                }
            if (input.x < 205)
                turn(false);
            else if (input.x >= 479)
                turn(true);
            else {
                draw_tools();
                overlay();
            }
        } else {
            for (const auto &hit : hits)
                if (hit.contains(input.x, input.y)) {
                    // A power choice requires the whole sampled contact to stay in its button.
                    if (view == View::PowerMenu && input.touch_bounds) {
                        const auto& bounds = *input.touch_bounds;
                        if (!hit.contains(bounds.min_x, bounds.min_y) ||
                            !hit.contains(bounds.max_x, bounds.max_y)) return;
                    }
                    int id = hit.id;
                    // A long press on a book in the library opens its details instead of the book.
                    if (input.long_press && view == View::Library && id == Tools) id = ResumeDetails;
                    if (input.long_press && view == View::Library && id >= EntryBase && id < TocBase &&
                        !entries[static_cast<std::size_t>(id - EntryBase)].directory)
                        id += DetailBase - EntryBase;
                    control(id);
                    break;
                }
        }
        return;
    }
    const auto action = map_input(context(), input.event.input);
    if (action == Action::ForceRefresh)
        force_refresh();
    else if (action == Action::PreviousPage)
        control(Prev);
    else if (action == Action::NextPage)
        control(Next);
    else if (action == Action::Close || action == Action::CancelPreview ||
             action == Action::Cancel)
        back();
}


} // namespace pico_page
