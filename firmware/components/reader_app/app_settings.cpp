#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::load_library_font(const std::string& path) {
    // Sharing a face avoids duplicating FreeType tables; labels retain their own sizes and emphasis.
    if (controlled_storage && body.valid() && !font_fallback && effective_font == path && effective_weight == "regular") {
        library_font.close();
        library_font_path = path; library_uses_body = true; library_font_reload = false;
        return;
    }
    if (library_font_path == path && controlled_storage && !library_font_reload && !library_uses_body) return;
    library_uses_body = false; library_font_reload = false;
    library_font_path = path;
    // A failed face remains invalid; each complete label then uses the UI face.
    if (!library_font.open(root + "/" + path)) library_font.close();
}


void ReaderApp::Impl::load_fonts(const ReaderSettings *preview) {
    const auto &selected = preview ? *preview : settings;
    const auto resolved = weight_font(selected);
    weight_unavailable = selected.font_weight == "light" && !light_available(selected);
    const auto tag = selected.font_weight == "bold" ? "-Bold." : selected.font_weight == "medium" ? "-Medium." : selected.font_weight == "light" ? "-Light." : "-Regular.";
    const bool native_weight = resolved.find(tag) != std::string::npos;
    const bool weighted_base = resolved.find("-Bold.") != std::string::npos || resolved.find("-Medium.") != std::string::npos || resolved.find("-Light.") != std::string::npos;
    weight_unavailable = weight_unavailable || (selected.font_weight != "regular" && !native_weight && weighted_base);
    body.set_synthetic_weight(native_weight || weight_unavailable ? 0 : selected.font_weight == "bold" ? 2 : selected.font_weight == "medium" ? 1 : 0);
    if (effective_font == resolved && body.valid() && controlled_storage) {
        if (effective_weight != selected.font_weight) { clear_body_cache(); clear_prefetch(); }
        effective_weight = selected.font_weight;
        if (!library_font_path.empty()) load_library_font(library_font_path);
        return;
    }
    clear_body_cache();
    clear_prefetch();
    font_identity.clear();
    font_scan.reset();
    font_fallback = false;
    builtin_body = false;
    const std::string path = root + "/" + resolved;
    if (!body.open(path)) {
        if (!body.open(root + "/Fonts/LXGWWenKai-Regular.ttf")) {
            body.open_fallback();
            builtin_body = true;
        }
        font_fallback = true;
    }
    effective_font = resolved;
    effective_weight = selected.font_weight;
    if (!library_font_path.empty()) load_library_font(library_font_path);
}


void ReaderApp::Impl::draw_font_picker() {
    const bool unavailable = font_fallback || weight_unavailable;
    canvas.fill_rect(0, 500, 684, 716, 15);
    canvas.line(0, 500, 683, 500, 1, 2);
    text("预览 · 尚未应用", 36, 514, 24, 350, 3);
    const auto label = font_name(draft.font);
    const auto width = std::min(220, text_width(label, 24));
    text(label, 648 - width, 514, 24, width, 3);
    canvas.line(0, 550, 683, 550, 1, 2);
    text("选择字体", 36, 576, 32, 350, 1, true);
    const auto count = "共 " + std::to_string(fonts.size()) + " 款";
    text(count, 648 - text_width(count, 24), 582, 24, 220);
    const auto pages = std::max<std::size_t>(1, (fonts.size() + fonts_per_screen - 1) / fonts_per_screen);
    font_picker_page = std::min(font_picker_page, pages - 1);
    for (std::size_t row = 0; row < fonts_per_screen; ++row) {
        const auto index = font_picker_page * fonts_per_screen + row;
        if (index >= fonts.size()) break;
        const int y = 624 + static_cast<int>(row) * 90;
        const auto& path = fonts[index];
        const bool current = path == edit_snapshot.config.settings.font;
        auto name = font_name(path);
        // Keep weight files distinguishable even when their family has a friendly name.
        if (name != basename(path)) {
            if (path.find("-Light.") != std::string::npos) name += " · 细";
            else if (path.find("-Medium.") != std::string::npos) name += " · 中粗";
            else if (path.find("-Bold.") != std::string::npos) name += " · 粗";
        }
        file_choice(FontEntryBase + static_cast<int>(index), name, "", y, path == draft.font, current ? "当前使用" : nullptr);
    }
    if (font_picker_page) button(Prev, "上一页", 36, 1008, 216, 88);
    else disabled_button("上一页", 36, 1008, 216, 88);
    center(std::to_string(font_picker_page + 1) + " / " + std::to_string(pages), 252, 1038, 180, 24);
    if (font_picker_page + 1 < pages) button(Next, "下一页", 432, 1008, 216, 88);
    else disabled_button("下一页", 432, 1008, 216, 88);
    button(FontCancel, "取消", 24, 1104, 312, 96);
    if (!unavailable) button(FontAccept, "选用此字体", 348, 1104, 312, 96, true);
    else disabled_button("选用此字体", 348, 1104, 312, 96);
}


void ReaderApp::Impl::font_picker_control(int id) {
    if (id == FontCancel || id == Back) cancel_font_picker();
    else if (id == FontAccept) {
        if (font_fallback || weight_unavailable) return;
        font_picker_open = false;
        draw_typography();
    } else if (id == Prev || id == Next) {
        const auto pages = (fonts.size() + fonts_per_screen - 1) / fonts_per_screen;
        if (id == Prev && font_picker_page) --font_picker_page;
        else if (id == Next && font_picker_page + 1 < pages) ++font_picker_page;
        else return;
        // Browsing leaves the completed preview intact, including any prepared image.
        hits.clear();
        draw_font_picker();
    } else if (id >= FontEntryBase && id < FontAccept) {
        const auto index = static_cast<std::size_t>(id - FontEntryBase);
        if (index >= fonts.size() || draft.font == fonts[index]) return;
        draft.font = fonts[index];
        draft.font_weight = "regular";
        reset_overrides = false;
        draw_typography();
    } else return;
    overlay();
}


void ReaderApp::Impl::draw_typography(bool prepare_images) {
    view = View::Typography;
    hits.clear();
    canvas.clear();
    load_fonts(&draft);
    const auto spec = layout(draft, 64, typography_preview_bottom());
    const auto preview = plan_page(offset, spec);
    if (!prepare_images && !preview.images.empty()) {
        text("预览暂不可用", 36, 96, 28);
    } else if (async_content && !image_prepared && preview.error.empty() && !preview.images.empty()) {
        // The worker prepares the image; the panel stays visible and only cancelling is accepted.
        begin_image(offset, preview, true);
        text("正在准备图片…", 36, 300, 26, 400, 3);
        button(Dismiss, "取消", 262, 368, 160, 64, false, 24);
        loading_cancel = hits.back();
    } else {
        if (!preview.error.empty() || !draw_page_content(preview, spec))
            text("预览暂不可用", 36, 96, 28);
    }
    if (font_picker_open) {
        if (font_fallback || weight_unavailable) {
            int clear_top = 430;
            for (const auto& line : preview.lines)
                if (line.top < clear_top && line.bottom > 430) clear_top = line.top;
            canvas.fill_rect(0, clear_top, 684, 500 - clear_top, 15);
            note(font_fallback ? "所选字体不可用，预览使用替代字体。" : kWeightUnavailableNotice, 446);
        }
        draw_font_picker();
        return;
    }
    const bool changed = typography_changed();
    tabs(656, Typography);
    canvas.fill_rect(24, 605, 636, 44, 15);
    text(changed ? "预览 · 尚未应用" : "当前排版 · 此书", 36, 614, 24, 432, 3);
    text(changed ? "放弃修改" : "收起", changed ? 540 : 600, 614, 24, 120);
    const std::array<int, 8> ids{{FontChoice, FontWeight, FontSize, LineHeight, Margin, Gap, Indent, Defaults}};
    auto name = [](int id) -> std::string {
        switch (id) {
        case FontChoice: return "字体";
        case FontWeight: return "字重";
        case FontSize: return "字号";
        case LineHeight: return "行距";
        case Margin: return "页边距";
        case Gap: return "段距";
        case Indent: return "首行缩进";
        default: return "恢复默认";
        }
    };
    auto value = [&](int id) -> std::string {
        switch (id) {
        case FontChoice: return font_name(draft.font);
        case FontWeight: return weight_name(draft.font_weight);
        case FontSize: return std::to_string(draft.font_size_px) + " px";
        case LineHeight: return decimal(draft.line_height);
        case Margin: return std::to_string(draft.margin_x_px) + " px";
        case Gap: return decimal(draft.paragraph_gap_lines) + " 行";
        default: return std::to_string(draft.indent_chars) + " 字";
        }
    };
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const int x = 24 + static_cast<int>(i % 4) * 162, y = 780 + static_cast<int>(i / 4) * 92;
        if (ids[i] == Defaults) {
            outline(x, y, 150, 80, 1);
            center("恢复默认", x, y + 21, 150, 24);
        } else {
            outline(x, y, 150, 80, parameter == ids[i] ? 3 : 1);
            center(name(ids[i]), x, y + 5, 150, 24);
            center(value(ids[i]), x + 6, y + 37, 138, 24, 1, true);
        }
        hits.push_back({x, y, 150, 80, ids[i]});
    }
    auto can_step = [&](int direction) {
        switch (parameter) {
        case FontWeight: return direction < 0 ? draft.font_weight != (light_available(draft) ? "light" : "regular") : draft.font_weight != "bold";
        case FontSize: return direction < 0 ? draft.font_size_px > 28 : draft.font_size_px < 56;
        case LineHeight: return direction < 0 ? draft.line_height > 1.35001 : draft.line_height < 1.64999;
        case Margin: return direction < 0 ? draft.margin_x_px > 24 : draft.margin_x_px < 52;
        case Gap: return direction < 0 ? draft.paragraph_gap_lines > 0.00001 : draft.paragraph_gap_lines < 0.99999;
        default: return direction < 0 ? draft.indent_chars > 0 : draft.indent_chars < 2;
        }
    };
    if (can_step(-1)) button(Minus, "−", 24, 976, 150, 88, false, 40);
    else disabled_button("−", 24, 976, 150, 88, 40);
    center(value(parameter), 186, 999, 312, 32, 1, true);
    if (can_step(1)) button(Plus, "+", 510, 976, 150, 88, false, 40);
    else disabled_button("+", 510, 976, 150, 88, 40);
    if (!font_fallback && !weight_unavailable) button(Scope, "设为阅读默认", 24, 1088, 312, 96);
    else disabled_button("设为阅读默认", 24, 1088, 312, 96);
    if (!font_fallback && !weight_unavailable && changed) button(Apply, "应用到此书", 348, 1088, 312, 96, true);
    else disabled_button(font_fallback || weight_unavailable ? "无法应用" : "已应用", 348, 1088, 312, 96);
    if (font_fallback || weight_unavailable) {
        canvas.fill_rect(24, preview_bottom - 70, 636, 58, 15);
        note(weight_unavailable && !font_fallback ? kWeightUnavailableNotice : "所选字体不可用，预览使用替代字体。", preview_bottom - 60);
    }
}


void ReaderApp::Impl::draw_reading_settings() {
    view = View::Refresh;
    hits.clear(); canvas.clear(); header("阅读", Back, "设置");
    const auto &values = edit_snapshot.config.settings;
    text("阅读默认值 · 单本书的调整不受影响", 36, 118, 24, 612, 5);
    text("排版", 36, 192, 24, 612, 5);
    canvas.line(36, 234, 647, 234, 12, 2);
    text("默认排版", 36, 250, 28, 440);
    text(typography_summary(values), 36, 294, 24, 440, 3);
    // The typography page previews and edits through the open book.
    if (has_book()) {
        text("›", 636, 268, 28, 24);
        hits.push_back({24, 234, 636, 108, Typography});
    } else text("打开书后可调整", 648 - text_width("打开书后可调整", 24), 272, 24, 200, 3);
    canvas.line(36, 341, 647, 341, 12, 2);
    text("刷新", 36, 374, 24, 612, 5);
    const std::array<std::pair<const char*, const char*>, 3> modes{{
        {"清晰", "每页执行全刷，减少残影"}, {"均衡", "灰阶更新，定期全刷"}, {"快速", "优先响应速度，可能更易留痕"}}};
    const std::array<const char*, 3> presets{{"clear", "balanced", "fast"}};
    for (int i = 0; i < 3; ++i) {
        const int y = 416 + i * 114;
        const bool selected = values.refresh_preset == presets[i];
        for (int row = -15; row <= 15; ++row)
            for (int col = -15; col <= 15; ++col) {
                const int radius = row * row + col * col;
                if ((radius <= 225 && radius >= 144) || (selected && radius <= 49)) canvas.pixel(51 + col, y + 56 + row, 1);
            }
        text(modes[i].first, 88, y + 17, 28, 540, 1, selected);
        text(modes[i].second, 88, y + 63, 24, 540, 3);
        canvas.line(36, y + 113, 647, y + 113, 12, 2);
        hits.push_back({24, y, 636, 114, RefreshClear + i});
    }
    const auto value_line = [&](int id, const std::string& title, const std::string& value, int y) {
        text(title, 36, y + 25, 28, 360);
        text(value, 648 - text_width(value, 24), y + 27, 24, 280, 3);
        canvas.line(36, y + 89, 647, y + 89, 12, 2);
        if (id) hits.push_back({24, y, 636, 90, id});
    };
    if (values.refresh_preset == "clear") value_line(0, "自动全刷间隔", "每页", 758);
    else value_line(RefreshInterval, "自动全刷间隔", refresh_interval_label(values) + " ›", 758);
    value_line(PageEffects, "翻页效果", std::string(page_effect == PageEffect::Off ? "关闭" : "水波纹") + " · 实验 ›", 848);
    value_line(DictionaryOpen, "离线词典", std::to_string(settings.dictionary_enabled.size()) + " 套已启用 ›", 938);
    button(FullRefresh, "立即全刷", 24, 1080, 636, 96);
}


void ReaderApp::Impl::draw_settings() {
    if (!bookmark_io_ready()) return;
    view = View::Settings;
    hits.clear(); canvas.clear(); header("设置", Back, "书库");
    const auto cfg = store.load_settings();
    const auto &values = cfg.settings;
    sync::Config account;
    std::string detail;
    const bool configured = sync::load_config(root, account, detail);
    std::uint64_t card_total = 0, card_free = 0;
    const bool measured = store.card_space(card_total, card_free);
    int y = 120;
    const auto row = [&](int id, const std::string& title, const std::string& value) {
        settings_row(id, title, value, y, 12, settings_pitch);
        y += settings_pitch;
    };
    const auto wifi = wifi_summary();
    row(Import, "网络与传输", (wifi.rfind("Wi-Fi", 0) == 0 || wifi.rfind("设备热点", 0) == 0 ? wifi : "Wi-Fi " + wifi) + " · " +
        (std::time(nullptr) >= provision::clock_floor ? "时间已校准" : "时间未校准"));
    row(SyncSettings, "KOReader 同步", configured ? server_name(account) + " · " + account.username : "未配置");
    row(Refresh, "阅读", font_name(values.font) + " " + std::to_string(values.font_size_px) + " · 行距 " +
        compact(values.line_height) + " · " + refresh_name(values) + "刷新");
    row(InputSettings, "书库", std::string(values.library_view == "grid" ? "卡片" : "列表") + "视图 · 文件夹" +
        (values.library_folders == "first" ? "置顶" : "跟随最近"));
    const auto sleep = values.sleep_screen == "reading" ? std::string("保留正文") : values.sleep_screen == "cover" ? std::string("显示封面") : "显示" + sleep_name(values);
    row(PowerSettings, "电源与休眠", "休眠" + sleep + " · " +
        (values.auto_sleep_minutes ? std::to_string(values.auto_sleep_minutes) + " 分钟后休眠" : "不自动休眠"));
    row(StorageSettings, "存储", measured ? "可用 " + file_size(card_free) : "可用空间未知");
    // Secondary entries: regular weight, 40 px below the groups.
    const auto minor = [&](int id, const std::string& title, const std::string& value, bool bold, int top) {
        text(title, 36, top + 21, 28, 400);
        if (!value.empty()) text(value, 628 - text_width(value, 24), top + 24, 24, 260, bold ? 1 : 3, bold);
        text("›", 636, top + 20, 28, 24);
        canvas.line(36, top + 81, 647, top + 81, 12, 2);
        hits.push_back({24, top, 636, 82, id});
    };
    const auto offer = update_offer();
    const bool newer = offer == update::Offer::Newer || offer == update::Offer::NewerSchema;
    const auto status = update_check.stage != CheckStage::None ? std::string("检查失败")
        : !update_state.manifest ? std::string("检查更新")
        : offer == update::Offer::Current || offer == update::Offer::Skipped ? std::string("已是最新")
        : offer == update::Offer::Development ? std::string("开发构建") : "新版本 " + update_state.manifest->version;
    minor(UpdateOpen, "固件更新", status, newer && update_check.stage == CheckStage::None, y + 40);
    minor(Help, "使用说明", "", false, y + 122);
    minor(About, "关于", "", false, y + 204);
    if (session.needs_save()) {
        // Unsaved progress outranks other notices; the whole footer retries the save.
        footer_bar("进度尚未保存 · 点此重试");
        hits.push_back({0, footer_zone, 684, 96, SaveRetry});
    } else footer_bar(cfg.diagnostics.empty() ? notice() : settings_file_problem(cfg.diagnostics) + "，请检查后重新打开。");
}


void ReaderApp::Impl::draw_help_demo() {
    const bool full = help_page == 3;
    if (full) {
        hits.clear();
        hits.push_back({0, 1016, 205, 112, Prev});
        hits.push_back({205, 1016, 274, 112, FullRefresh});
        hits.push_back({479, 1016, 205, 112, Next});
        help_regions(0, 0, 684, 1216);
        const std::array<const char*, 3> names{{"上一页", "打开工具", "下一页"}};
        for (int i = 0; i < 3; ++i) {
            const int x = i == 0 ? 0 : i == 1 ? 205 : 479, w = i == 1 ? 274 : 205;
            const auto ink = help_zone == HelpPrevious + i ? 15 : 1;
            center(names[i], x, 370, w, 28, ink, true);
            center(i == 1 ? "K2 全刷" : i == 0 ? "K1 上一屏" : "K3 下一屏", x, 1024, w, 22, ink);
        }
        canvas.fill_rect(24, 620, 636, 196, 15); outline(24, 620, 636, 196);
        center("点按示例 · 不改变阅读位置", 36, 646, 612, 26, 1, true);
        center("点任一区域，查看对应操作", 36, 700, 612, 24);
        center("按 K1 / K3 或左右轻扫离开", 36, 754, 612, 24);
        const char* positions[] = {"第 12 / 340 页", "3.53%", "第 2 / 10 章", "第一章 从一页开始", ""};
        text(positions[help_position], 36, 1154, 24, 476, help_zone == HelpPosition ? 15 : 1);
        const char* devices[] = {"84%", "12:00", ""};
        center(devices[help_device], 540, 1154, 144, 24, help_zone == HelpDevice ? 15 : 1);
        return;
    }
    help_regions(36, 276, 274, 486);
    const char* labels[] = {"左侧：上一页", "中部：打开工具", "右侧：下一页", "页脚左：切换位置", "页脚右：切换状态"};
    for (int i = 0; i < 5; ++i) {
        const int id = HelpPrevious + i, y = 290 + i * 88;
        if (help_zone == id) canvas.fill_rect(330, y, 330, 72, 1);
        text(labels[i], 342, y + 22, 24, 306, help_zone == id ? 15 : 1);
        hits.push_back({330, y, 330, 88, id});
    }
    text("点示意图或说明，查看对应区域", 36, 786, 24, 612, 3);
    for (int i = 0; i < 3; ++i) {
        const int x = 24 + i * 212;
        const bool selected = help_key == HelpKey1 + i;
        if (selected) canvas.fill_rect(x, 842, 212, 88, 1);
        center("K" + std::to_string(i + 1), x, 869, 212, 28, selected ? 15 : 1, selected);
        hits.push_back({x, 842, 212, 88, HelpKey1 + i});
    }
    outline(24, 842, 636, 88);
    canvas.fill_rect(236, 842, 2, 88, 1);
    canvas.fill_rect(448, 842, 2, 88, 1);
    const char* description = help_key == HelpKey2 ? "所有可交互页面：立即全刷。" :
        help_key == HelpKey1 ? "正文：上一页。书库、目录、书签、说明：上一屏。" : "正文：下一页。书库、目录、书签、说明：下一屏。";
    wrapped(description, 36, 942, 612, 24);
    text(help_key == HelpKey2 ? "休眠准备时忽略 K2，不全刷。" : "工具、排版和键盘中不翻动正文。", 36, 1030, 24, 612, 3);
    footer_bar("", help_page, kHelpPages.size());
}


void ReaderApp::Impl::begin_storage(bool clear_cache) {
    abandon_content();
    if (counting_job) counting_job->cancel();
    counting_session.reset(); font_scan.reset();
    clear_prefetch();
    if (clear_cache) {
        covers.clear(); dictionary_session.reset();
        custom_prepare_finished = false; custom_prepare_attempts = 0; custom_sleep_image.reset();
    }
    storage_usage = {};
    storage_usage.roots = settings.library_roots;
    message.clear();
    ContentJob::Request request;
    request.kind = clear_cache ? ContentJob::Kind::ClearCache : ContentJob::Kind::StorageScan;
    request.root = root; request.generation = foreground_generation;
    pending_content = std::move(request);
    draw_storage();
    loading(clear_cache ? "正在清理缓存…" : "正在统计存储空间…");
}

void ReaderApp::Impl::draw_storage() {
    view = View::StorageSettings;
    hits.clear(); canvas.clear(); header("存储", Back, "设置");
    const auto& roots = storage_usage.roots;
    const auto size = [](const std::optional<std::uint64_t> &bytes) { return bytes ? file_size(*bytes) : std::string("未知"); };
    text("TF 卡", 36, 128, 22, 612, 3);
    value_row(0, "书籍原文件", "", size(storage_usage.books), 164, 70);
    value_row(0, "缓存（可重建）", "", size(storage_usage.cache), 234, 70);
    value_row(0, "可用空间", "", size(storage_usage.available), 304, 70, true);
    text("文件位置", 36, 402, 22, 612, 3);
    value_row(0, "书籍与文章", "", roots.size() == 1 ? roots.front() + "/" : "多个目录（见设置文件）", 438, 70);
    value_row(0, "自行添加的字体", "", "Fonts/", 508, 70);
    value_row(0, "设备设置", "", "PicoPage/settings.toml", 578, 70);
    value_row(0, "阅读进度", "", "原书旁的 .picopage/", 648, 70);
    note(notice().empty() ? "文章可放扫描目录下的 Articles/；保留 PicoPage/。" : message, 950);
    note("保留书籍、设置和阅读进度。", 988);
    note("清理后首次开书或查词可能较慢。", 1026);
    button(ClearCache, "清理全部缓存", 24, 1080, 636, 96);
}


} // namespace pico_page
