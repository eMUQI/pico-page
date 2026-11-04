#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::draw_wifi() {
    if (!portal_live && !bookmark_io_ready()) return;
    view = View::WifiSettings;
    hits.clear(); canvas.clear();
    if (portal_live) {
        header("Wi-Fi", Back, "传输");
        text(wifi_summary(), 36, 132, 34, 612, 1, true);
        wrapped("查看状态不会结束传书。更换网络或关闭 Wi-Fi 前，请先结束传书。", 36, 220, 612, 26);
        button(PortalStop, portal_closing ? "正在结束…" : "结束传书", 24, 900, 636, 96);
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
    const auto notice_text = !wifi_notice.empty() ? wifi_notice : !readable ? "网络配置无效，请检查 PicoPage/network.toml。"
        : wifi_status.detail;
    const auto saved_count = std::to_string(wifi_saved.size());
    switch (wifi_page) {
    case WifiPage::Home: {
        header("Wi-Fi", Back, wifi_from_sync ? "同步" : wifi_from_transfer ? "传输" : "设置");
        const bool enabled = wifi_enabled(), online = wifi_online();
        text(wifi_summary(), 36, 132, 34, 612, 1, true);
        text(!enabled ? "同步和经已保存网络传书不会联网"
             : !pinned_wifi_ssid.empty() ? "指定" + quoted_name(pinned_wifi_ssid) + "，不改用其他网络"
             : online ? "自动 · 信号最强" : "自动连接信号最强的已保存网络", 36, 186, 22, 612, 3);
        value_row(WifiToggle, "Wi-Fi", "", enabled ? "开" : "关", 236, 104, true, false);
        if (enabled) {
            value_row(WifiModeOpen, "连接方式", "", pinned_wifi_ssid.empty() ? "自动" : "指定", 340);
            value_row(WifiManage, "已保存网络", "", saved_count + " 个", 444);
            value_row(WifiAdd, "添加网络", "", "", 548);
            if (online || wifi_status.phase == WifiPhase::Connecting ||
                (wifi_worker && wifi_kind == WifiOperation::Kind::Connect))
                button(WifiDisconnect, "断开", 24, 700, 636, 96);
            else if (wifi_saved.empty()) disabled_button("立即连接", 24, 700, 636, 96);
            else button(WifiConnect, "立即连接", 24, 700, 636, 96);
            if (!online)
                wrapped("同步和传书会自动联网，用完断开。立即连接后保持在线，直到断开或休眠。", 36, 824, 612, 24, true, 3);
        }
        const bool calibrating = wifi_worker && wifi_kind == WifiOperation::Kind::Clock;
        const auto now = std::time(nullptr);
        auto clock_value = now >= provision::clock_floor ? reading_footer::clock(now, 8 * 60) : "尚未校准";
        const bool can_calibrate = enabled && !wifi_saved.empty() && !wifi_worker && !active_network;
        const int clock_action = calibrating && network::live(wifi_hold) ? WifiClockCancel
                               : can_calibrate ? WifiClock : 0;
        if (calibrating) clock_value += network::live(wifi_hold) ? " · 取消" : " · 结束中";
        else if (can_calibrate) clock_value += " · 校准";
        const auto clock_detail = calibrating ? "UTC+8 · 正在获取网络时间"
                                : !enabled ? "UTC+8 · 打开 Wi-Fi 后可校准"
                                : wifi_saved.empty() ? "UTC+8 · 添加 Wi-Fi 后可校准"
                                : "UTC+8 · 联网时自动校准";
        canvas.line(36, 916, 647, 916, 12, 2);
        value_row(clock_action, "时间", clock_detail, clock_value, 916, 104, false, false);
        wrapped(notice_text, 36, 1060, 612, 24);
        break;
    }
    case WifiPage::Mode:
        header("连接方式", Back, wifi_entry_page == WifiPage::Mode ? "同步" : "Wi-Fi");
        choice(WifiModeAuto, "自动", "信号最强的已保存网络，连不上时试下一个", 130, pinned_wifi_ssid.empty(), false, false);
        text("指定", 36, 272, 22, 612, 3);
        if (wifi_saved.empty()) note("还没有已保存的网络。", 316);
        for (std::size_t i = 0; i < wifi_saved.size(); ++i)
            choice(WifiModeBase + static_cast<int>(i), wifi_saved[i].ssid, "只连这个网络",
                   312 + static_cast<int>(i) * 124, wifi_saved[i].ssid == pinned_wifi_ssid, false, false);
        wrapped("指定后只连这个网络。它被忘记后回到自动。", 36,
                336 + static_cast<int>(std::max<std::size_t>(wifi_saved.size(), 1)) * 124, 612, 24, true, 3);
        wrapped(notice_text, 36, 1060, 612, 24);
        break;
    case WifiPage::Saved:
        header("已保存网络", Back, "Wi-Fi");
        note("已保存 " + saved_count + " 个，最多 " + std::to_string(provision::saved_limit) + " 个", 130);
        for (std::size_t i = 0; i < wifi_saved.size(); ++i)
            entry_row(WifiSavedBase + static_cast<int>(i), wifi_saved[i].ssid, "",
                      wifi_saved[i].ssid == pinned_wifi_ssid ? "指定" : "", 180 + static_cast<int>(i) * 112, 112);
        if (!full) button(WifiAdd, "添加网络", 24, 212 + static_cast<int>(wifi_saved.size()) * 112, 636, 96);
        wrapped(notice_text, 36, 1060, 612, 24);
        break;
    case WifiPage::Detail:
        header("网络", Back, "已保存网络");
        title_lines(wifi_choice, 36, 132, 612, 34, true, 1);
        text(wifi_choice == pinned_wifi_ssid ? "指定网络" : "已保存", 36, 186, 22, 612, 3);
        button(WifiUpdatePassword, "更新密码", 24, 260, 636, 96);
        button(WifiForget, "忘记网络", 24, 380, 636, 96);
        wrapped(notice_text, 36, 1060, 612, 24);
        break;
    case WifiPage::Forget:
        header("忘记网络", 0);
        title_lines("忘记" + quoted_name(wifi_choice) + "？", 36, 160, 612, 34, true, 2);
        wrapped(wifi_choice == pinned_wifi_ssid ? "已保存的密码会删除。连接方式将改回自动连接。" : "已保存的密码会删除。",
                36, 280, 612, 28);
        button(Back, "取消", 24, 1080, 306, 96, true);
        button(WifiForgetConfirm, "忘记网络", 354, 1080, 306, 96);
        break;
    case WifiPage::Add: {
        header("添加网络", Back, "Wi-Fi");
        const bool scanning = wifi_worker && wifi_kind == WifiOperation::Kind::Scan;
        note(full ? "已保存 " + saved_count + " 个，需先忘记一个" : "附近的 2.4 GHz 网络", 130);
        static constexpr std::size_t per_screen = 6;
        const auto screens = std::max<std::size_t>(1, (wifi_nearby.size() + per_screen - 1) / per_screen);
        wifi_add_page = std::min(wifi_add_page, screens - 1);
        if (scanning) note("正在扫描…", 208);
        else if (wifi_nearby.empty() && wifi_scanned) note("没有发现网络。", 208);
        const auto first = wifi_add_page * per_screen;
        for (std::size_t i = first; i < std::min(wifi_nearby.size(), first + per_screen); ++i) {
            const auto& found = wifi_nearby[i];
            const bool saved = known(found.ssid);
            value_row(saved || !full ? WifiNearbyBase + static_cast<int>(i) : 0, found.ssid, "",
                      saved ? "已保存" : found.open ? "开放网络" : "需要密码", 176 + static_cast<int>(i - first) * 92, 92);
        }
        value_row(full ? 0 : WifiManualName, "手动输入网络名称", "", "", 728, 92);
        wrapped("没看到？网络可能只有 5 GHz，或手机热点还没打开。", 36, 840, 612, 22, true, 3);
        if (scanning) disabled_button("正在扫描…", 24, 920, 636, 96);
        else button(WifiRescan, "重新扫描", 24, 920, 636, 96);
        footer_bar(notice_text, wifi_add_page, screens);
        break;
    }
    case WifiPage::Joining:
        header("连接 Wi-Fi", 0);
        title_lines("正在连接" + quoted_name(join_draft.ssid), 36, 160, 612, 34, true, 2);
        wrapped("连接成功后才会保存这个网络。", 36, 280, 612, 26);
        button(WifiJoinCancel, "取消", 24, 1000, 636, 96);
        note(join_open ? "取消后回到网络列表。" : "取消后回到密码输入，已输入的内容保留。", 1124);
        break;
    case WifiPage::JoinFailed: {
        header("连接 Wi-Fi", 0);
        title_lines((join_unsaved ? "没能保存" : "无法连接") + quoted_name(join_draft.ssid), 36, 160, 612, 34, true, 2);
        const int height = wrapped(join_unsaved ? "已经连上这个网络，但无法写入 TF 卡。下次开机不会自动连接。"
                                                : join_error + "网络没有保存。", 36, 280, 612, 26);
        const bool password = join_failure == LinkFailure::Password && !join_open;
        if (password) wrapped("密码区分大小写。可点「显示」逐位核对。", 36, 300 + height, 612, 24, true, 3);
        button(password ? WifiJoinEdit : WifiJoinRetry, password ? "重新输入密码" : "重试", 24, 880, 636, 96, true);
        button(WifiJoinList, join_origin == WifiPage::Detail ? "返回已保存网络" : "返回网络列表", 24, 1000, 636, 96);
        break;
    }
    case WifiPage::Joined:
        header("连接 Wi-Fi", 0);
        title_lines("已连接" + quoted_name(join_draft.ssid), 36, 160, 612, 34, true, 2);
        wrapped("网络已保存，现在共 " + saved_count + " 个。", 36, 280, 612, 26);
        if (!badge_configured) {
            wrapped("还没有设置 KOReader 同步。", 36, 336, 612, 24, true, 3);
            button(SyncSettings, "设置 KOReader 同步", 24, 880, 636, 96);
        }
        button(WifiJoinDone, "完成", 24, 1000, 636, 96, true);
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
    if (entry.layer == TextEntry::Layer::Letters) keys.push_back({entry.shift ? "小写" : "大写", "", KeyKind::Shift, left, third, 94, height});
    else keys.push_back({entry.layer == TextEntry::Layer::Digits ? "#+=" : "123", "", entry.layer == TextEntry::Layer::Digits ? KeyKind::Symbols : KeyKind::Digits, left, third, 94, height});
    characters(row[2], left + 100, third);
    keys.push_back({"退格", "", KeyKind::Backspace, 580, third, 94, height});
    keys.push_back({entry.layer == TextEntry::Layer::Letters ? "123" : "abc", "", entry.layer == TextEntry::Layer::Letters ? KeyKind::Digits : KeyKind::Letters, left, fourth, 128, height});
    if (entry_url() && entry.layer == TextEntry::Layer::Letters) {
        keys.push_back({"https://", "https://", KeyKind::Character, 144, fourth, 130, height});
        int x = 280;
        for (const char* value : {".", "/", ":"}) {
            keys.push_back({value, value, KeyKind::Character, x, fourth, 62, height});
            x += 68;
        }
    } else keys.push_back({"空格", " ", KeyKind::Character, 144, fourth, 334, height});
    keys.push_back({entry.field == TextEntry::Field::WifiPassword ? "连接" : entry.field == TextEntry::Field::WifiName ? "下一步" : "完成",
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
        title = "输入密码"; prompt = quoted_name(join_draft.ssid) + "的密码";
        hint = entry.value.empty() ? (join_secured ? "区分大小写，至少 8 位。" : "区分大小写，至少 8 位；开放网络留空。")
             : entry.value.size() < 8 ? "还差 " + std::to_string(8 - entry.value.size()) + " 位才能连接。"
             : !entry_ready() ? "64 位的密码只能是十六进制密钥，否则最多 63 位。" : "区分大小写。";
        break;
    case TextEntry::Field::WifiName:
        title = "网络名称"; prompt = "第 1 步，共 2 步"; hint = "区分大小写。下一步填写密码；开放网络留空。";
        break;
    case TextEntry::Field::Endpoint:
        title = "服务地址"; prompt = "KOReader 同步服务地址"; hint = "自建服务填写完整地址，含端口。";
        break;
    case TextEntry::Field::Username: title = "账户"; prompt = "KOReader 同步账户"; hint = "区分大小写。"; break;
    default: title = "密码"; prompt = "KOReader 同步密码"; hint = "设备只保存密码的 MD5，与 KOReader 相同。";
    }
    if (entry.value.size() >= entry_limit() && entry_ready()) hint = "已到长度上限。";
    header(title, Back, wifi ? "网络" : "同步");
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
    button(EntryClear, "清空", 540, 284, 120, 88);
    if (entry_secret()) button(EntryMask, entry.masked ? "显示" : "隐藏", 408, 284, 120, 88);
    else if (entry_url()) button(EntryDefault, "恢复默认", 380, 284, 148, 88);
    text(hint, 36, 372, 24, 612, 3);
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
