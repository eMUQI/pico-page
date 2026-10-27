#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::layout_notes(const std::string& markdown, bool truncated, int first_top) {
    note_lines.clear(); note_starts.clear();
    auto blocks = update::parse_notes(markdown);
    if (blocks.empty()) blocks.push_back({update::NoteBlock::Kind::Text, {{"这一版没有附更新说明。", false}}});
    if (truncated) blocks.push_back({update::NoteBlock::Kind::Text, {{"日志过长，后面的内容未显示。", false}}});
    for (const auto& block : blocks) {
        const bool heading = block.kind == update::NoteBlock::Kind::Heading;
        const bool item = block.kind == update::NoteBlock::Kind::Item;
        NoteLine line{{}, {}, heading ? 30 : 26, item ? 28 : 0, heading ? 28 : 12, item, 0};
        ui.set_pixel_size(line.size);
        std::u32string scalars;
        std::vector<bool> bold;
        for (const auto& span : block.spans)
            for (auto scalar : decode_utf8(span.text)) { scalars.push_back(scalar); bold.push_back(heading || span.bold); }
        const int width = 612 - line.indent;
        const auto first = note_lines.size();
        int used = 0;
        for (std::size_t i = 0; i < scalars.size(); ++i) {
            const int advance = ui.advance(scalars[i]), run = run_width(ui, scalars, i);
            const bool closing = std::u32string_view(U"，。、；：！？）」』”’》").find(scalars[i]) != std::u32string_view::npos;
            if (used && !closing && (used + advance > width || (run <= width && used + run > width))) {
                const NoteLine next{{}, {}, line.size, line.indent, line.gap, false, 0};
                note_lines.push_back(std::exchange(line, next));
                used = 0;
            }
            if (!used && note_lines.size() > first && scalars[i] == U' ') continue;
            line.text.push_back(scalars[i]); line.bold.push_back(bold[i]);
            used += advance;
        }
        note_lines.push_back(std::move(line));
        note_lines[first].block_rows = note_lines.size() - first;
    }
    // A block moves to the next screen whole when it fits there; a longer one is divided between
    // lines. A heading also takes the first line of the block after it along.
    int y = first_top;
    note_starts.push_back(0);
    for (std::size_t i = 0; i < note_lines.size(); ++i) {
        const auto& line = note_lines[i];
        const int height = note_line_height(line.size);
        const auto after = i + line.block_rows;
        const int block = static_cast<int>(line.block_rows) * height + (line.size == 30 && after < note_lines.size()
            ? note_lines[after].gap + note_line_height(note_lines[after].size) : 0);
        const bool top = i == note_starts.back();
        const int gap = line.block_rows && !top ? line.gap : 0;
        if (!top && y + gap + (line.block_rows && block <= notes_bottom - notes_top ? block : height) > notes_bottom) {
            note_starts.push_back(i);
            y = notes_top;
        } else y += gap;
        y += height;
    }
    note_page = std::min(note_page, note_starts.size() - 1);
}


void ReaderApp::Impl::draw_update_home(update::Offer offer, int preview_bottom) {
    const auto& manifest = update_state.manifest;
    header("固件更新", Back, "设置");
    std::string problem;
    const bool networks = !wifi_targets(problem).networks.empty();
    const bool enabled = wifi_enabled();
    const auto check_button = [&](int y, const char* label) {
        if (networks && enabled) button(UpdateCheck, label, 24, y, 636, 88);
        else button(WifiSettings, enabled ? "先设置 Wi-Fi ›" : "先打开 Wi-Fi ›", 24, y, 636, 88);
    };
    const auto development = update::development_build(running_version);
    if (update_check.stage != CheckStage::None &&
        (!manifest || offer == update::Offer::Current || offer == update::Offer::Skipped)) {
        update_head(development ? "当前版本 · 开发构建" : "当前版本", running_version);
        canvas.line(36, 300, 647, 300, 1, 2);
        canvas.fill_rect(36, 328, 48, 48, 1);
        center("!", 36, 334, 48, 32, 15, true);
        text("这次没有检查成功", 100, 328, 34, 548, 1, true);
        const int body = wrapped(update_check.body, 36, 398, 612, 26);
        check_cells(398 + body + 40, update_check.stage, update_check.ssid, update_check.note);
        if (update_notice != update_check.body) wrapped(update_notice, 36, 900, 612, 24);
        if (manifest) {
            const auto at = update_checked_at();
            // The cache keeps the manifest and the time, not how it compared with the firmware then.
            text("上次成功检查" + (at.empty() ? "时间未知" : " " + at) + " · 服务器版本 " + manifest->version,
                 36, 1010, 22, 612, 3);
        }
        if (!networks || !enabled) { check_button(1080, "检查更新"); return; }
        const bool wifi = update_check.stage == CheckStage::Wifi;
        const bool server = update_check.stage == CheckStage::Server;
        button(wifi ? UpdateCheck : server ? Back : WifiSettings, wifi ? "重新检查" : server ? "稍后" : "Wi-Fi 设置", 24, 1080, 306, 96);
        button(wifi ? WifiSettings : UpdateCheck, wifi ? "Wi-Fi 设置" : "重新检查", 354, 1080, 306, 96, true);
        return;
    }
    if (!manifest || offer == update::Offer::Current || offer == update::Offer::Skipped) {
        update_head(development ? "当前版本 · 开发构建" : "当前版本", running_version);
        canvas.line(36, 300, 647, 300, 1, 2);
        const bool checked_now = update_fresh && offer == update::Offer::Current;
        text(!manifest ? "尚未检查更新" : offer == update::Offer::Skipped ? "没有可安装的更新"
             : checked_now ? "已是最新版本" : "上次检查未发现新版本", 36, 328, 34, 612, 1, true);
        int y = 398;
        if (manifest) {
            const auto at = update_checked_at();
            text(checked_now && !at.empty() ? "检查于 " + at : update_checked(), 36, y, 26, 612);
            y += 80;
        }
        if (offer == update::Offer::Skipped)
            y += wrapped(manifest->version + " 上次没有启动成功，已跳过。服务器发布更高的版本后可以更新。", 36, y, 612, 26) + 24;
        if (manifest && manifest->version == running_version) value_row(UpdateNotes, "本版更新日志", "", "", y, 96);
        wrapped(update_notice, 36, 940, 612, 24);
        check_button(1080, manifest ? "重新检查" : "检查更新");
        return;
    }
    if (note_lines.empty()) layout_notes(manifest->notes, manifest->notes_truncated, notes_first_top);
    if (offer == update::Offer::Development) {
        update_head("当前版本 · 开发构建", running_version);
        canvas.line(36, 300, 647, 300, 1, 2);
        text("服务器版本", 36, 324, 24, 612, 3);
        text(manifest->version, 36, 362, 36, 612, 1, true);
        wrapped("开发构建无法判断新旧。安装后会换成服务器上的发布版本，本机改动不保留。", 36, 432, 612, 26);
        value_row(UpdateNotes, "更新日志", "", "", 716, 96);
    } else {
        update_head("新版本", manifest->version, "当前 " + running_version +
            (manifest->published_at.size() == 10 ? " · 发布于 " + manifest->published_at.substr(5) : "") + " · " + file_size(manifest->size));
        canvas.line(36, 300, 647, 300, 1, 2);
        draw_note_lines(0, 324, preview_bottom);
        value_row(UpdateNotes, "完整更新日志", "", note_starts.size() > 1 ? std::to_string(note_starts.size()) + " 屏" : "", 716, 96);
    }
    check_button(880, "重新检查");
    update_install_button(992, offer);
    const auto reason = offer == update::Offer::TooLarge ? "新固件 " + file_size(manifest->size) + "，超过本机 4 MiB 的固件分区，不能安装。这与存储卡空间无关。"
        : offer == update::Offer::OlderSchema ? std::string("这个版本写入的数据格式比本机旧，不能安装。") : update_block();
    wrapped(update_notice.empty() ? reason : update_notice, 36, 1104, 612, 24);
}


void ReaderApp::Impl::draw_update() {
    view = View::Update;
    const auto& manifest = update_state.manifest;
    const auto offer = update_offer();
    if (update_page == UpdatePage::Result && update_rolled_back) {
        int y = update_sheet(700, "更新没有完成");
        y += wrapped(update_to + " 没有启动成功，已回到 " + running_version + "。阅读进度没有变化。", 36, y, 612, 26) + 16;
        wrapped("设备会跳过这个版本，直到服务器发布更高的版本。", 36, y, 612, 24, true, 3);
        button(UpdateDismiss, "知道了", 24, 1088, 636, 96, true);
        return;
    }
    // Pages that describe a manifest fall back to the home page when it is gone.
    if (!manifest && update_page != UpdatePage::Checking && update_page != UpdatePage::Result) update_page = UpdatePage::Home;
    if (update_page == UpdatePage::Home && update_ready && manifest && manifest->version == update_to && update_installable(offer))
        update_page = UpdatePage::Ready;
    hits.clear(); canvas.clear();
    const auto megabytes = [](std::uint32_t bytes) {
        char value[16];
        std::snprintf(value, sizeof(value), "%.1f", bytes / 1048576.0);
        return std::string(value);
    };
    switch (update_page) {
    case UpdatePage::Result: {
        header("已更新", 0);
        const bool described = manifest && manifest->version == update_to;
        if (note_lines.empty()) layout_notes(described ? manifest->notes : "", described && manifest->notes_truncated, 284);
        if (!note_page) {
            text(update_from + " →", 36, 136, 24, 612, 3);
            text(update_to, 36, 176, 44, 612, 1, true);
            canvas.line(36, 256, 647, 256, 1, 2);
        }
        draw_note_lines(note_page, note_page ? notes_top : 284);
        button(UpdateDismiss, "回到书库", 24, 1008, 636, 96, true);
        footer_bar("", note_page, note_starts.size());
        return;
    }
    case UpdatePage::Notes:
        header(manifest->version, Back, "返回");
        if (note_lines.empty()) layout_notes(manifest->notes, manifest->notes_truncated, notes_first_top);
        if (!note_page)
            text((manifest->published_at.empty() ? "" : "发布于 " + manifest->published_at + " · ") + update_stage(manifest->version),
                 36, notes_top, 24, 612, 3);
        draw_note_lines(note_page, note_page ? notes_top : notes_first_top);
        if (update_installable(offer)) update_install_button(1008, offer);
        footer_bar("", note_page, note_starts.size());
        return;
    case UpdatePage::Checking: {
        header("固件更新", 0);
        update_head("当前版本", running_version);
        canvas.line(36, 300, 647, 300, 1, 2);
        text(update_cancelling ? "正在取消" : "正在检查", 36, 328, 34, 612, 1, true);
        // The worker publishes Connecting while it joins, Clock while it reaches the internet
        // and Request for the manifest; the cells follow the latest phase shown.
        const auto phase = wifi_status.phase;
        const bool linked = phase == WifiPhase::Connected || phase == WifiPhase::Clock || phase == WifiPhase::Request;
        const bool requesting = phase == WifiPhase::Request;
        const auto ssid = linked ? wifi_status.ssid : "";
        const auto named = ssid.empty() ? std::string("Wi-Fi") : quoted_name(ssid);
        text(!linked ? "正在连接 Wi-Fi。" : requesting ? "已连接互联网，正在读取更新信息。" : "已连接" + named + "，正在访问互联网。",
             36, 398, 26, 612);
        check_cell(36, 456, "Wi-Fi", linked ? CellState::Done : CellState::Active, linked ? "已连接" : "连接中…", ssid);
        check_cell(244, 456, "互联网", requesting ? CellState::Done : linked ? CellState::Active : CellState::Waiting,
                   requesting ? "正常" : linked ? "连接中…" : "等待");
        check_cell(452, 456, "更新服务器", requesting ? CellState::Active : CellState::Waiting, requesting ? "读取中…" : "等待");
        if (update_cancelling) disabled_button("正在取消…", 24, 1080, 636, 96);
        else button(UpdateCancel, "取消", 24, 1080, 636, 96);
        return;
    }
    case UpdatePage::Downloading: {
        header("固件更新", 0);
        const auto total = manifest->size, bytes = std::min(update_bytes, total);
        update_head(update_cancelling ? "正在取消" : bytes >= total ? "正在校验" : "正在下载", manifest->version);
        outline(36, 320, 612, 40);
        canvas.fill_rect(36, 320, static_cast<int>(612ull * bytes / total), 40, 1);
        text(megabytes(bytes) + " / " + file_size(total), 36, 376, 26, 400);
        const auto percent = std::to_string(100ull * bytes / total) + "%";
        text(percent, 648 - text_width(percent, 26), 376, 26, 120);
        update_steps(bytes >= total ? 1 : 0);
        wrapped("下载期间不会休眠。可以取消，当前固件不受影响。", 36, 556, 612, 24, true, 3);
        if (update_cancelling) disabled_button("正在取消…", 24, 1088, 636, 88);
        else button(UpdateCancel, "取消下载", 24, 1088, 636, 88);
        return;
    }
    case UpdatePage::Failed: {
        header("固件更新", Back, "设置");
        const char* title = "无法写入固件";
        std::string body = "设备无法写入更新分区。";
        switch (update_failure) {
        case UpdateFailure::Link: title = "没有连上 Wi-Fi"; body = update_notice; break;
        case UpdateFailure::Internet: title = "无法开始下载"; body = "已连接 Wi-Fi，但" + update_notice + "。请确认这个网络可以上网。"; break;
        case UpdateFailure::Network:
            // The same failure covers a refused connection before any byte and a later interruption.
            if (update_bytes) { title = "下载中断"; body = "下载到 " + megabytes(update_bytes) + " MiB 时连接中断或超时。"; }
            else { title = "无法开始下载"; body = "没有连上固件下载服务器。"; }
            break;
        case UpdateFailure::Server: title = "下载服务器返回错误"; body = "没有取得固件文件，稍后再试。"; break;
        case UpdateFailure::Mismatch: title = "校验没有通过"; body = "下载的文件与服务器公布的不一致，已丢弃。"; break;
        case UpdateFailure::TooLarge: title = "固件超出容量"; body = "新固件 " + file_size(manifest->size) + "，超过本机 4 MiB 的固件分区，无法安装。这与存储卡空间无关。"; break;
        case UpdateFailure::WrongImage: title = "型号不符"; body = "这个固件不是给 Read Pico 的，或版本与服务器公布的不符。"; break;
        default: break;
        }
        // The same file would fail again; only transfer and write failures are worth repeating.
        const bool repeatable = update_failure != UpdateFailure::TooLarge && update_failure != UpdateFailure::WrongImage;
        canvas.fill_rect(36, 140, 48, 48, 1);
        center("!", 36, 146, 48, 32, 15, true);
        text(title, 100, 144, 34, 548, 1, true);
        wrapped(body + "仍在使用 " + running_version + "。", 36, 228, 612, 26);
        if (repeatable) button(UpdateRetry, "重新下载", 24, 976, 636, 96, true);
        button(UpdateLater, repeatable ? "稍后" : "返回", 24, 1088, 636, 88);
        return;
    }
    case UpdatePage::Ready:
        header("固件更新", 0);
        update_head("已下载并校验", update_to);
        update_steps(2);
        wrapped("重启期间不要断电。重启前保存阅读位置，新固件启动后回到书库。", 36, 556, 612, 26);
        wrapped(update_notice.empty() ? "选“稍后”回到设置。设备关机或重启后需要重新下载。" : update_notice, 36, 700, 612, 24, true, 3);
        button(UpdateRestart, "重启并安装", 24, 976, 636, 96, true);
        button(UpdateLater, "稍后", 24, 1088, 636, 88);
        return;
    default:
        draw_update_home(offer, update_page == UpdatePage::Schema ? 590 : 700);
        if (update_page != UpdatePage::Schema) return;
        const bool progress = manifest->progress_schema > update::progress_schema;
        const bool config = manifest->config_schema > update::config_schema;
        int y = update_sheet(640, progress && config ? "阅读进度和设置格式会变化" : progress ? "阅读进度格式会变化" : "设置文件格式会变化");
        y += wrapped("新版本按新格式保存。之后如果回到 " + running_version + "，新格式的" +
                     (progress && config ? "阅读进度和设置" : progress ? "阅读进度" : "设置") + "不会显示。", 36, y, 612, 26) + 16;
        wrapped("详见本版更新日志的“兼容性”一节。", 36, y, 612, 24, true, 3);
        button(UpdateConfirm, "我知道了，安装", 24, 976, 636, 96, true);
        button(UpdateCancel, "取消", 24, 1088, 636, 88);
    }
}


void ReaderApp::Impl::update_begin(UpdateOperation::Kind kind) {
    std::string problem;
    auto targets = wifi_targets(problem);
    update_notice.clear();
    if (!wifi_enabled()) update_notice = "Wi-Fi 已关闭。请先在 Wi-Fi 设置中打开。";
    else if (active_network || portal_live || wifi_worker) update_notice = "请先结束当前任务。";
    else if (targets.networks.empty()) update_notice = problem;
    if (!update_notice.empty()) update_page = UpdatePage::Home;
    else {
        const bool download = kind == UpdateOperation::Kind::Download;
        update_hold = std::make_shared<std::atomic_bool>(false);
        update_active = ++update_sequence;
        UpdateOperation operation{kind, update_active, update::manifest_url, 0, {}, {}, std::move(targets), update_hold};
        if (download) {
            const auto& manifest = *update_state.manifest;
            operation.url = manifest.url; operation.size = manifest.size;
            operation.sha256 = manifest.sha256; operation.version = manifest.version;
            update_ready = false;
        }
        update_request = std::move(operation);
        update_cancelling = false; update_bytes = 0; update_step = 0;
        update_page = download ? UpdatePage::Downloading : UpdatePage::Checking;
    }
    draw_update(); overlay();
}


void ReaderApp::Impl::update_complete(std::uint64_t id, UpdateFailure failure, int status, const std::string& body, const std::string& detail,
                     LinkFailure link, const std::string& ssid) {
    if (!id || id != update_worker) return;
    update_worker = 0;
    const bool cancelled = std::exchange(update_cancelling, false) || failure == UpdateFailure::Cancelled || id != update_active;
    const bool download = update_page == UpdatePage::Downloading;
    const bool confirming = std::exchange(update_confirming, false);
    update_active = 0;
    network::release(update_hold);
    if (view != View::Update || (update_page != UpdatePage::Downloading && update_page != UpdatePage::Checking)) {
        if (update_page == UpdatePage::Downloading || update_page == UpdatePage::Checking) update_page = UpdatePage::Home;
        return;
    }
    update_notice.clear();
    update_page = UpdatePage::Home;
    if (cancelled) update_notice = download ? "已取消下载，当前固件没有变化。" : "";
    else if (download && failure == UpdateFailure::None) {
        update_ready = true;
        update_to = update_state.manifest->version;
        update_page = UpdatePage::Ready;
    } else if (download) {
        update_failure = failure;
        update_notice = detail;
        update_page = UpdatePage::Failed;
    } else {
        update::Manifest manifest;
        std::string problem;
        update_check = {};
        if (failure == UpdateFailure::Link) {
            std::string ignored;
            const auto targets = wifi_targets(ignored).networks;
            const auto absent = targets.size() > 1 ? "已保存 " + std::to_string(targets.size()) + " 个网络，附近一个也没找到。"
                                : targets.size() == 1 ? "附近没有找到" + quoted_name(targets.front().ssid) + "。" : std::string();
            update_check = {CheckStage::Wifi, "",
                link == LinkFailure::NotNearby ? "附近没有" : link == LinkFailure::Password ? "密码错误" : "",
                link == LinkFailure::NotNearby && !absent.empty() ? absent + "可以换个位置再试，或者添加新的网络。" : detail};
        } else if (failure == UpdateFailure::Internet) {
            update_check = {CheckStage::Internet, ssid, "失败", "已连接" + (ssid.empty() ? std::string("Wi-Fi") : quoted_name(ssid)) +
                "，但" + detail + "。请确认这个网络可以上网，稍后再试。"};
        } else if (failure != UpdateFailure::None || status != 200 || !update::parse_manifest(body, manifest, problem)) {
            update_check = {CheckStage::Server, ssid,
                failure != UpdateFailure::None ? "连接失败" : status != 200 ? "HTTP " + std::to_string(status) : "内容无效",
                failure != UpdateFailure::None ? "已连接互联网，但无法连接更新服务器。稍后再试。"
                // The update host answers 404 until the first manifest is published.
                : status == 404 ? "更新服务器上还没有发布固件。"
                : status != 200 ? "更新服务器返回 HTTP " + std::to_string(status) + "，暂时无法检查。稍后再试。"
                : "更新服务器返回的内容无法识别。稍后再试。"};
        }
        // Pages that offer an installation show the cause below their controls.
        if (update_check.stage != CheckStage::None) update_notice = update_check.body;
        else {
            const auto later = update::compare_versions(manifest.version, update_state.skipped);
            if (later && *later > 0) update_state.skipped.clear();
            const auto now = static_cast<std::int64_t>(std::time(nullptr));
            const auto& shown = update_state.manifest;
            const bool same = shown && shown->version == manifest.version && shown->sha256 == manifest.sha256 &&
                              shown->url == manifest.url && shown->size == manifest.size;
            update_state.manifest = std::move(manifest);
            update_fresh = true;
            update_state.checked_at = now >= provision::clock_floor ? now : 0;
            note_lines.clear(); note_page = 0;
            if (!save_update_state()) update_notice = "检查结果没有写入 TF 卡，下次需要重新检查。";
            if (confirming && !same) update_notice = "更新信息有变化，请确认后再安装。";
            const auto offer = update_offer();
            if (confirming && same && update_installable(offer) && update_block().empty()) {
                if (!update::requires_schema_confirmation(*update_state.manifest)) { update_begin(UpdateOperation::Kind::Download); return; }
                update_page = UpdatePage::Schema;
            }
        }
    }
    if (!display.busy()) { draw_update(); overlay(); }
}


bool ReaderApp::Impl::update_control(int id) {
    const auto offer = update_offer();
    const bool startable = update_installable(offer) && update_block().empty() && !update_waiting();
    switch (id) {
    case UpdateCheck:
        if (update_page == UpdatePage::Home && !update_waiting()) update_begin(UpdateOperation::Kind::Check);
        return true;
    case UpdateNotes:
        if (update_page != UpdatePage::Home || !update_state.manifest) return true;
        notes_return = update_page; update_page = UpdatePage::Notes; note_page = 0;
        break;
    case UpdateStart:
        if ((update_page != UpdatePage::Home && update_page != UpdatePage::Notes) || !update_installable(offer) ||
            update_waiting()) return true;
        if (!startable) break;
        // A manifest read from the card is confirmed with the server before it directs a download.
        if (!update_fresh) {
            std::string problem;
            if (!wifi_enabled() || wifi_targets(problem).networks.empty()) {
                control(WifiSettings);
                return true;
            }
            update_begin(UpdateOperation::Kind::Check);
            update_confirming = update_waiting();
            return true;
        }
        if (!update::requires_schema_confirmation(*update_state.manifest)) { update_begin(UpdateOperation::Kind::Download); return true; }
        update_page = UpdatePage::Schema;
        break;
    case UpdateConfirm:
        if (update_page == UpdatePage::Schema && startable && update_fresh) update_begin(UpdateOperation::Kind::Download);
        return true;
    case UpdateRetry:
        if (update_page != UpdatePage::Failed) return true;
        update_page = UpdatePage::Home; update_notice.clear();
        if (startable && update_fresh) { update_begin(UpdateOperation::Kind::Download); return true; }
        break;
    case UpdateCancel:
        if (update_page == UpdatePage::Schema) update_page = UpdatePage::Home;
        else if (update_page == UpdatePage::Checking || update_page == UpdatePage::Downloading) update_cancel();
        else return true;
        break;
    case UpdateLater:
        if (update_page == UpdatePage::Failed) { update_page = UpdatePage::Home; update_notice.clear(); break; }
        if (update_page == UpdatePage::Ready) { update_notice.clear(); draw_settings(); overlay(); }
        return true;
    case UpdateRestart:
        if (update_page != UpdatePage::Ready || !update_ready) return true;
        // The download may have drained the battery or the charger may have been removed.
        update_notice = update_battery_block();
        if (!update_notice.empty()) break;
        power_selected = PowerAction::Restart;
        power_request = {PowerAction::Restart, false, true};
        return true;
    case UpdateDismiss:
        if (update_page != UpdatePage::Result) return true;
        if (update_rolled_back) update_state.skipped = update_to;
        update_state.pending_from.clear(); update_state.pending_to.clear();
        // An unsaved dismissal only repeats this report at the next start.
        save_update_state();
        update_page = UpdatePage::Home; update_rolled_back = false;
        note_lines.clear(); note_page = 0;
        draw_library(); overlay();
        return true;
    case Prev:
    case Next:
        if (update_page != UpdatePage::Notes && (update_page != UpdatePage::Result || update_rolled_back)) return true;
        if (id == Prev && note_page) --note_page;
        else if (id == Next && note_page + 1 < note_starts.size()) ++note_page;
        else return true;
        break;
    default:
        return false;
    }
    draw_update(); overlay();
    return true;
}


} // namespace pico_page
