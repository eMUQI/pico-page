#include "app_impl.h"
#include "clock_panel.h"

namespace pico_page {

void ReaderApp::Impl::draw_wifi() {
    if (!portal_live && !bookmark_io_ready()) return;
    view = View::WifiSettings;
    hits.clear(); canvas.clear();
    if (portal_live) {
        header("Wi-Fi", Back, tr("传输"));
        text(wifi_summary(), 36, 132, 34, 612, 1, true);
        wrapped(tr("查看状态不会结束传书。更换网络或关闭 Wi-Fi 前，请先结束传书。"), 36, 220, 612, 26);
        button(PortalStop, portal_closing ? tr("正在结束…") : tr("结束传书"), 24, 900, 636, 96);
        return;
    }
    std::string detail;
    std::optional<provision::SavedWifi> pinned;
    const bool readable = provision::load_networks(root, wifi_saved, detail) &&
        provision::load_selected_network(root, pinned, detail);
    pinned_wifi_ssid = pinned ? pinned->ssid : "";
    const bool full = wifi_saved.size() >= provision::saved_limit;
    const auto known = [&](const std::string& ssid) {
        return std::any_of(wifi_saved.begin(), wifi_saved.end(), [&](const auto& wifi) { return wifi.ssid == ssid; });
    };
    const auto notice_text = !wifi_notice.empty() ? wifi_notice : !readable ? tr("网络配置无效，请检查 PicoPage/network.toml。")
        : std::string(tr(wifi_status.detail.c_str()));
    const auto saved_count = std::to_string(wifi_saved.size());
    switch (wifi_page) {
    case WifiPage::Home: {
        header("Wi-Fi", Back, wifi_from_sync ? tr("同步") : wifi_from_transfer ? tr("传输") : tr("设置"));
        const bool enabled = wifi_enabled(), online = wifi_online();
        text(wifi_summary(), 36, 132, 34, 612, 1, true);
        text(!enabled ? tr("同步和经已保存网络传书不会联网")
             : !pinned_wifi_ssid.empty() ? trf("指定「{network}」，不改用其他网络", {{"network", pinned_wifi_ssid}})
             : online ? tr("自动 · 信号最强") : tr("自动连接信号最强的已保存网络"), 36, 186, 22, 612, 3);
        value_row(WifiToggle, "Wi-Fi", "", enabled ? tr("开") : tr("关"), 236, 104, true, false);
        if (enabled) {
            value_row(WifiModeOpen, tr("连接方式"), "", pinned_wifi_ssid.empty() ? tr("自动") : tr("指定"), 340);
            value_row(WifiManage, tr("已保存网络"), "", trf("{count} 个", {{"count", saved_count}}), 444);
            value_row(WifiAdd, tr("添加网络"), "", "", 548);
            if (online || wifi_status.phase == WifiPhase::Connecting ||
                (wifi_worker && wifi_kind == WifiOperation::Kind::Connect))
                button(WifiDisconnect, tr("断开"), 24, 700, 636, 96);
            else if (wifi_saved.empty()) disabled_button(tr("立即连接"), 24, 700, 636, 96);
            else button(WifiConnect, tr("立即连接"), 24, 700, 636, 96);
            if (!online)
                wrapped(tr("同步和传书会自动联网，用完断开。立即连接后保持在线，直到断开或休眠。"), 36, 824, 612, 24, true, 3);
        }
        const bool calibrating = clock_running();
        const auto now = std::time(nullptr);
        const bool clock_valid = clock_time::valid(now);
        auto clock_value = clock_valid ? reading_footer::clock(now, clock_time::utc8_offset_seconds / 60) : tr("尚未校准");
        const bool can_calibrate = enabled && !wifi_saved.empty() && !wifi_worker && !active_network;
        const int clock_action = calibrating && network::live(wifi_hold) ? WifiClockCancel
                               : can_calibrate ? WifiClock : 0;
        if (calibrating) clock_value += network::live(wifi_hold) ? tr(" · 取消") : tr(" · 结束中");
        else if (can_calibrate) clock_value += tr(" · 校准");
        std::string clock_detail = calibrating ? tr("UTC+8 · 正在获取网络时间")
            : !enabled ? tr("UTC+8 · 打开 Wi-Fi 后可校准")
            : wifi_saved.empty() ? tr("UTC+8 · 添加 Wi-Fi 后可校准")
            : tr("UTC+8 · 联网时自动校准");
        if (!calibrating && enabled && !wifi_saved.empty() && clock_valid && clock_time::valid(last_clock_sync_at)) {
            const auto last = static_cast<std::time_t>(last_clock_sync_at);
            const auto last_time = clock_panel::local_date(last) + " " + reading_footer::clock(last, clock_time::utc8_offset_seconds / 60);
            clock_detail = trf("UTC+8 · 上次 {time}", {{"time", last_time}});
        }
        canvas.line(36, 916, 647, 916, 12, 2);
        value_row(clock_action, tr("时间"), clock_detail, clock_value, 916, 104, false, false);
        wrapped(notice_text, 36, 1060, 612, 24);
        break;
    }
    case WifiPage::Mode:
        header(tr("连接方式"), Back, wifi_entry_page == WifiPage::Mode ? tr("同步") : "Wi-Fi");
        choice(WifiModeAuto, tr("自动"), tr("信号最强的已保存网络，连不上时试下一个"), 130, pinned_wifi_ssid.empty(), false, false);
        text(tr("指定"), 36, 272, 22, 612, 3);
        if (wifi_saved.empty()) note(tr("还没有已保存的网络。"), 316);
        for (std::size_t i = 0; i < wifi_saved.size(); ++i)
            choice(WifiModeBase + static_cast<int>(i), wifi_saved[i].ssid, tr("只连这个网络"),
                   312 + static_cast<int>(i) * 124, wifi_saved[i].ssid == pinned_wifi_ssid, false, false);
        wrapped(tr("指定后只连这个网络。它被忘记后回到自动。"), 36,
                336 + static_cast<int>(std::max<std::size_t>(wifi_saved.size(), 1)) * 124, 612, 24, true, 3);
        wrapped(notice_text, 36, 1060, 612, 24);
        break;
    case WifiPage::Saved:
        header(tr("已保存网络"), Back, "Wi-Fi");
        note(trf("已保存 {count} 个，最多 {limit} 个", {{"count", saved_count}, {"limit", std::to_string(provision::saved_limit)}}), 130);
        for (std::size_t i = 0; i < wifi_saved.size(); ++i)
            entry_row(WifiSavedBase + static_cast<int>(i), wifi_saved[i].ssid, "",
                      wifi_saved[i].ssid == pinned_wifi_ssid ? tr("指定") : "", 180 + static_cast<int>(i) * 112, 112);
        if (!full) button(WifiAdd, tr("添加网络"), 24, 212 + static_cast<int>(wifi_saved.size()) * 112, 636, 96);
        wrapped(notice_text, 36, 1060, 612, 24);
        break;
    case WifiPage::Detail:
        header(tr("网络"), Back, tr("已保存网络"));
        title_lines(wifi_choice, 36, 132, 612, 34, true, 1);
        text(wifi_choice == pinned_wifi_ssid ? tr("指定网络") : tr("已保存"), 36, 186, 22, 612, 3);
        button(WifiUpdatePassword, tr("更新密码"), 24, 260, 636, 96);
        button(WifiForget, tr("忘记网络"), 24, 380, 636, 96);
        wrapped(notice_text, 36, 1060, 612, 24);
        break;
    case WifiPage::Forget:
        header(tr("忘记网络"), 0);
        title_lines(trf("忘记「{network}」？", {{"network", wifi_choice}}), 36, 160, 612, 34, true, 2);
        wrapped(wifi_choice == pinned_wifi_ssid ? tr("已保存的密码会删除。连接方式将改回自动连接。") : tr("已保存的密码会删除。"),
                36, 280, 612, 28);
        button(Back, tr("取消"), 24, 1080, 306, 96, true);
        button(WifiForgetConfirm, tr("忘记网络"), 354, 1080, 306, 96);
        break;
    case WifiPage::Add: {
        header(tr("添加网络"), Back, "Wi-Fi");
        const bool scanning = wifi_worker && wifi_kind == WifiOperation::Kind::Scan;
        note(full ? trf("已保存 {count} 个，需先忘记一个", {{"count", saved_count}}) : tr("附近的 2.4 GHz 网络"), 130);
        static constexpr std::size_t per_screen = 6;
        const auto screens = std::max<std::size_t>(1, (wifi_nearby.size() + per_screen - 1) / per_screen);
        wifi_add_page = std::min(wifi_add_page, screens - 1);
        if (scanning) note(tr("正在扫描…"), 208);
        else if (wifi_nearby.empty() && wifi_scanned) note(tr("没有发现网络。"), 208);
        const auto first = wifi_add_page * per_screen;
        for (std::size_t i = first; i < std::min(wifi_nearby.size(), first + per_screen); ++i) {
            const auto& found = wifi_nearby[i];
            const bool saved = known(found.ssid);
            value_row(saved || !full ? WifiNearbyBase + static_cast<int>(i) : 0, found.ssid, "",
                      saved ? tr("已保存") : found.open ? tr("开放网络") : tr("需要密码"), 176 + static_cast<int>(i - first) * 92, 92);
        }
        value_row(full ? 0 : WifiManualName, tr("手动输入网络名称"), "", "", 728, 92);
        wrapped(tr("没看到？网络可能只有 5 GHz，或手机热点还没打开。"), 36, 840, 612, 22, true, 3);
        if (scanning) disabled_button(tr("正在扫描…"), 24, 920, 636, 96);
        else button(WifiRescan, tr("重新扫描"), 24, 920, 636, 96);
        footer_bar(notice_text, wifi_add_page, screens);
        break;
    }
    case WifiPage::Joining:
        header(tr("连接 Wi-Fi"), 0);
        title_lines(trf("正在连接「{network}」", {{"network", join_draft.ssid}}), 36, 160, 612, 34, true, 2);
        wrapped(tr("连接成功后才会保存这个网络。"), 36, 280, 612, 26);
        button(WifiJoinCancel, tr("取消"), 24, 1000, 636, 96);
        title_lines(join_open ? tr("取消后回到网络列表。") : tr("取消后回到密码输入，已输入的内容保留。"), 36, 1124, 612, 24, false, 2, 3);
        break;
    case WifiPage::JoinFailed: {
        header(tr("连接 Wi-Fi"), 0);
        title_lines(trf(join_unsaved ? "没能保存「{network}」" : "无法连接「{network}」", {{"network", join_draft.ssid}}), 36, 160, 612, 34, true, 2);
        const int height = wrapped(join_unsaved ? tr("已经连上这个网络，但无法写入 TF 卡。下次开机不会自动连接。")
                                                : trf("{error}网络没有保存。", {{"error", join_error}}), 36, 280, 612, 26);
        const bool password = join_failure == LinkFailure::Password && !join_open;
        if (password) wrapped(tr("密码区分大小写。可点「显示」逐位核对。"), 36, 300 + height, 612, 24, true, 3);
        button(password ? WifiJoinEdit : WifiJoinRetry, password ? tr("重新输入密码") : tr("重试"), 24, 880, 636, 96, true);
        button(WifiJoinList, join_origin == WifiPage::Detail ? tr("返回已保存网络") : tr("返回网络列表"), 24, 1000, 636, 96);
        break;
    }
    case WifiPage::Joined:
        header(tr("连接 Wi-Fi"), 0);
        title_lines(trf("已连接「{network}」", {{"network", join_draft.ssid}}), 36, 160, 612, 34, true, 2);
        wrapped(trf("网络已保存，现在共 {count} 个。", {{"count", saved_count}}), 36, 280, 612, 26);
        if (!badge_configured) {
            wrapped(tr("还没有设置 KOReader 同步。"), 36, 336, 612, 24, true, 3);
            button(SyncSettings, tr("设置 KOReader 同步"), 24, 880, 636, 96);
        }
        button(WifiJoinDone, tr("完成"), 24, 1000, 636, 96, true);
        break;
    }
}

std::vector<ReaderApp::Impl::Key> ReaderApp::Impl::keyboard() const {
    static constexpr const char* rows[3][3] = {{"qwertyuiop", "asdfghjkl", "zxcvbnm"},
                                               {"1234567890", "-/:;()$&@\"", ".,?!'_%"},
                                               {"[]{}#^*+=\\", "|~<>`", ""}};
    static constexpr int width = 61, pitch = 67, height = 92, left = 10, first = keyboard_top + 16;
    std::vector<Key> keys;
    const auto characters = [&](const char* values, int x, int y) {
        for (; *values; ++values, x += pitch) {
            const char value = entry.shift && entry.layer == TextEntry::Layer::Letters ? static_cast<char>(*values - 32) : *values;
            keys.push_back({std::string(1, value), std::string(1, value), KeyKind::Character, x, y, width, height});
        }
    };
    const auto row = rows[static_cast<std::size_t>(entry.layer)];
    characters(row[0], left, first);
    characters(row[1], left + (10 - static_cast<int>(std::strlen(row[1]))) * pitch / 2, first + 100);
    const int third = first + 200, fourth = first + 300;
    if (entry.layer == TextEntry::Layer::Letters) keys.push_back({entry.shift ? tr("小写") : tr("大写"), "", KeyKind::Shift, left, third, 94, height});
    else keys.push_back({entry.layer == TextEntry::Layer::Digits ? "#+=" : "123", "", entry.layer == TextEntry::Layer::Digits ? KeyKind::Symbols : KeyKind::Digits, left, third, 94, height});
    characters(row[2], left + 100, third);
    keys.push_back({tr("退格"), "", KeyKind::Backspace, 580, third, 94, height});
    keys.push_back({entry.layer == TextEntry::Layer::Letters ? "123" : "abc", "", entry.layer == TextEntry::Layer::Letters ? KeyKind::Digits : KeyKind::Letters, left, fourth, 128, height});
    if (entry_url() && entry.layer == TextEntry::Layer::Letters) {
        keys.push_back({"https://", "https://", KeyKind::Character, 144, fourth, 130, height});
        int x = 280;
        for (const char* value : {".", "/", ":"}) {
            keys.push_back({value, value, KeyKind::Character, x, fourth, 62, height});
            x += 68;
        }
    } else keys.push_back({tr("空格"), " ", KeyKind::Character, 144, fourth, 334, height});
    keys.push_back({entry.field == TextEntry::Field::WifiPassword ? tr("连接") : entry.field == TextEntry::Field::WifiName ? tr("下一步") : tr("完成"),
                    "", KeyKind::Enter, 484, fourth, 190, height});
    return keys;
}

void ReaderApp::Impl::draw_entry() {
    view = View::TextEntry;
    hits.clear(); canvas.clear();
    const bool wifi = entry.field == TextEntry::Field::WifiPassword || entry.field == TextEntry::Field::WifiName;
    std::string title, prompt, hint;
    switch (entry.field) {
    case TextEntry::Field::WifiPassword:
        title = tr("输入密码"); prompt = trf("「{network}」的密码", {{"network", join_draft.ssid}});
        hint = entry.value.empty() ? (join_secured ? tr("区分大小写，至少 8 位。") : tr("区分大小写，至少 8 位；开放网络留空。"))
             : entry.value.size() < 8 ? trf("还差 {count} 位才能连接。", {{"count", std::to_string(8 - entry.value.size())}})
             : !entry_ready() ? tr("64 位的密码只能是十六进制密钥，否则最多 63 位。") : tr("区分大小写。");
        break;
    case TextEntry::Field::WifiName:
        title = tr("网络名称"); prompt = tr("第 1 步，共 2 步"); hint = tr("区分大小写。下一步填写密码；开放网络留空。");
        break;
    case TextEntry::Field::Endpoint:
        title = tr("服务地址"); prompt = tr("KOReader 同步服务地址"); hint = tr("自建服务填写完整地址，含端口。");
        break;
    case TextEntry::Field::Username: title = tr("账户"); prompt = tr("KOReader 同步账户"); hint = tr("区分大小写。"); break;
    default: title = tr("密码"); prompt = tr("KOReader 同步密码"); hint = tr("设备只保存密码的 MD5，与 KOReader 相同。");
    }
    if (entry.value.size() >= entry_limit() && entry_ready()) hint = tr("已到长度上限。");
    header(title, Back, wifi ? tr("网络") : tr("同步"));
    text(prompt, 36, 132, 24, 612, 3);
    outline(24, 176, 636, 88);
    ui.set_pixel_size(34);
    const auto scalars = decode_utf8(entry.value);
    const std::u32string shown = entry.masked ? std::u32string(scalars.size(), U'•') : scalars;
    std::size_t begin = shown.size();
    for (int used = ui.advance(U'|'); begin > 0 && used + ui.advance(shown[begin - 1]) <= 596; --begin)
        used += ui.advance(shown[begin - 1]);
    int x = 44;
    const int baseline = 200 + ui.metrics().ascender;
    for (std::size_t i = begin; i < shown.size(); ++i) {
        ui.draw(canvas, shown[i], x, baseline, 1, false);
        x += ui.advance(shown[i]);
    }
    ui.draw(canvas, U'|', x, baseline, 5, false);
    button(EntryClear, tr("清空"), 540, 284, 120, 88);
    if (entry_secret()) button(EntryMask, entry.masked ? tr("显示") : tr("隐藏"), 408, 284, 120, 88);
    else if (entry_url()) button(EntryDefault, tr("恢复默认"), 380, 284, 148, 88);
    title_lines(hint, 36, 396, 612, 24, false, 3, 3);
    canvas.fill_rect(0, keyboard_top, 684, 3, 1);
    const auto keys = keyboard();
    for (const auto& key : keys) {
        const bool enter = key.kind == KeyKind::Enter;
        const int size = key.kind == KeyKind::Character && key.text.size() == 1 && key.text != " " ? 34 : 24;
        if (enter && !entry_ready()) {
            disabled_button(key.label, key.x, key.y, key.w, key.h, size);
            continue;
        }
        if (enter) canvas.fill_rect(key.x, key.y, key.w, key.h, 1);
        else outline(key.x, key.y, key.w, key.h);
        center(key.label, key.x, key.y + (key.h - size) / 2 - 3, key.w, size, enter ? 15 : 1);
    }
    entry.field_dirty = entry.keys_dirty = false;
    entry.enter_key_drawn_enabled = entry_ready();
}

bool ReaderApp::Impl::key_input(const AppInput& input) {
    if (!input.touch) return false;
    const auto keys = keyboard();
    const auto inside = [](const Key& key, int x, int y) {
        return x >= key.x - 3 && x < key.x + key.w + 3 && y >= key.y - 4 && y < key.y + key.h + 4;
    };
    const auto found = std::find_if(keys.begin(), keys.end(), [&](const Key& key) { return inside(key, input.x, input.y); });
    if (found == keys.end()) return false;
    if (input.touch_bounds && (!inside(*found, input.touch_bounds->min_x, input.touch_bounds->min_y) ||
                               !inside(*found, input.touch_bounds->max_x, input.touch_bounds->max_y))) return true;
    const auto& key = *found;
    switch (key.kind) {
    case KeyKind::Character:
        if (entry.value.size() + key.text.size() > entry_limit()) { entry.field_dirty = true; break; }
        entry.value += key.text;
        entry.field_dirty = true;
        if (entry.shift) { entry.shift = false; entry.keys_dirty = true; }
        break;
    case KeyKind::Backspace:
        if (entry.value.empty()) break;
        while (entry.value.size() > 1 && (static_cast<unsigned char>(entry.value.back()) & 0xC0) == 0x80) entry.value.pop_back();
        entry.value.pop_back();
        entry.field_dirty = true;
        break;
    case KeyKind::Shift: entry.shift = !entry.shift; entry.keys_dirty = true; break;
    case KeyKind::Letters: entry.layer = TextEntry::Layer::Letters; entry.keys_dirty = true; break;
    case KeyKind::Digits: entry.layer = TextEntry::Layer::Digits; entry.shift = false; entry.keys_dirty = true; break;
    case KeyKind::Symbols: entry.layer = TextEntry::Layer::Symbols; entry.keys_dirty = true; break;
    case KeyKind::Enter:
        if (!display.busy()) entry_done();
        return true;
    }
    if (entry.keys_dirty && !display.busy()) flush_entry();
    return true;
}

void ReaderApp::Impl::entry_done() {
    const auto field = entry.field;
    const auto value = entry.value;
    if (!entry_ready()) return;
    switch (field) {
    case TextEntry::Field::WifiName:
        join_draft.ssid = value;
        open_entry(TextEntry::Field::WifiPassword, join_draft.password);
        return;
    case TextEntry::Field::WifiPassword:
        join_draft.password = value;
        join_wifi();
        return;
    case TextEntry::Field::Endpoint: {
        auto endpoint = value.empty() ? std::string(provision::default_endpoint) : value;
        while (endpoint.size() > 8 && endpoint.back() == '/') endpoint.pop_back();
        account.changed = account.changed || endpoint != account.endpoint;
        account.endpoint = endpoint;
        break;
    }
    case TextEntry::Field::Username:
        account.changed = account.changed || value != account.username;
        account.username = value;
        break;
    default:
        account.changed = account.changed || !value.empty();
        account.password = value;
    }
    entry = {};
    draw_sync_settings(); overlay();
}

}
