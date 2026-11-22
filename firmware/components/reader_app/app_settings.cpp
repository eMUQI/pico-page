#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::load_library_font(const std::string& path) {
    if (controlled_storage && body.valid() && !font_fallback && effective_font == path && effective_weight == "regular") {
        library_font.close();
        library_font_path = path; library_uses_body = true; library_font_reload = false;
        return;
    }
    if (library_font_path == path && controlled_storage && !library_font_reload && !library_uses_body) return;
    library_uses_body = false; library_font_reload = false;
    library_font_path = path;
    if (!library_font.open(root + "/" + path)) library_font.close();
}

void ReaderApp::Impl::load_fonts(const ReaderSettings *preview) {
    const auto &selected = preview ? *preview : settings;
    const bool author_weight = selected.original_epub && !txt_name(book_path);
    const std::string weight = author_weight ? "regular" : selected.font_weight;
    const auto resolved = author_weight ? selected.font : weight_font(selected);
    weight_unavailable = weight == "light" && !light_available(selected);
    const auto tag = weight == "bold" ? "-Bold." : weight == "medium" ? "-Medium." : weight == "light" ? "-Light." : "-Regular.";
    const bool native_weight = resolved.find(tag) != std::string::npos;
    const bool weighted_base = resolved.find("-Bold.") != std::string::npos || resolved.find("-Medium.") != std::string::npos || resolved.find("-Light.") != std::string::npos;
    weight_unavailable = weight_unavailable || (weight != "regular" && !native_weight && weighted_base);
    body.set_synthetic_weight(native_weight || weight_unavailable ? 0 : weight == "bold" ? 2 : weight == "medium" ? 1 : 0);
    if (effective_font == resolved && body.valid() && controlled_storage) {
        if (effective_weight != weight) { clear_body_cache(); clear_prefetch(); }
        effective_weight = weight;
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
    effective_weight = weight;
    if (!library_font_path.empty()) load_library_font(library_font_path);
}

void ReaderApp::Impl::draw_font_picker() {
    const bool unavailable = font_fallback || weight_unavailable;
    canvas.fill_rect(0, 500, 684, 716, 15);
    canvas.line(0, 500, 683, 500, 1, 2);
    text(tr("预览 · 尚未应用"), 36, 514, 24, 350, 3);
    const auto label = font_name(draft.font);
    const auto width = std::min(220, text_width(label, 24));
    text(label, 648 - width, 514, 24, width, 3);
    canvas.line(0, 550, 683, 550, 1, 2);
    text(tr("选择字体"), 36, 576, 32, 350, 1, true);
    const auto count = trf("共 {count} 款", {{"count", std::to_string(fonts.size())}});
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
        if (name != basename(path)) {
            if (path.find("-Light.") != std::string::npos) name += tr(" · 细");
            else if (path.find("-Medium.") != std::string::npos) name += tr(" · 中粗");
            else if (path.find("-Bold.") != std::string::npos) name += tr(" · 粗");
        }
        file_choice(FontEntryBase + static_cast<int>(index), name, "", y, path == draft.font, current ? tr("当前使用") : nullptr);
    }
    constexpr int pagination_top = 1008, pagination_height = 88, page_number_size = 24;
    const int page_number_top = top_for_middle(ui, page_number_size, pagination_top + pagination_height / 2);
    if (font_picker_page) button(Prev, tr("上一页"), 36, pagination_top, 216, pagination_height);
    else disabled_button(tr("上一页"), 36, pagination_top, 216, pagination_height);
    center(std::to_string(font_picker_page + 1) + " / " + std::to_string(pages), 252, page_number_top, 180, page_number_size);
    if (font_picker_page + 1 < pages) button(Next, tr("下一页"), 432, pagination_top, 216, pagination_height);
    else disabled_button(tr("下一页"), 432, pagination_top, 216, pagination_height);
    button(FontCancel, tr("取消"), 24, 1104, 312, 96);
    if (!unavailable) button(FontAccept, tr("选用此字体"), 348, 1104, 312, 96, true);
    else disabled_button(tr("选用此字体"), 348, 1104, 312, 96);
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


void ReaderApp::Impl::change_epub_layout() {
    if (txt_name(book_path)) return;
    draft.original_epub = !draft.original_epub;
    reset_overrides = false;
    typography_preview.reset();
    typography_preview_failed = false;
    if (draft.original_epub == settings.original_epub) { draw_typography(); return; }

    Anchor anchor;
    if (!chapter.anchor_at(static_cast<std::uint32_t>(offset), anchor)) {
        typography_preview_failed = true;
        draw_typography(false);
        return;
    }
    if (!async_content) {
        Chapter preview;
        std::uint32_t position = 0;
        std::string detail;
        TypographyPreview prepared;
        const auto holds = [&](const Chapter& candidate) { return candidate.offset_for(anchor, position); };
        if (find_content(store, document, chapter_index, preview, detail,
                         draft.original_epub ? ChapterLoad::Original : ChapterLoad::Reading, holds, {}, &prepared.cache)) {
            prepared.fonts = prepare_embedded_fonts(store, document, preview, prepared.fonts_stable);
            prepared.chapter = std::move(preview);
            typography_preview = std::move(prepared);
            typography_preview_offset = position;
        } else typography_preview_failed = true;
        draw_typography();
        return;
    }
    auto reader = std::move(chapter_session);
    const auto reader_bytes = chapter_stream_bytes;
    abandon_content();
    yield_counting_session();
    release_counting();
    chapter_session = reader ? std::move(reader) : std::make_shared<ChapterSession>();
    chapter_stream_bytes = reader_bytes;
    ContentJob::Request request;
    request.kind = ContentJob::Kind::Chapter; request.target = ContentJob::Target::Anchor;
    request.root = root; request.path = root + "/" + book_path; request.identity = document.metadata();
    request.book_index = verified_reader_index();
    request.chapter = chapter_index; request.anchor = std::move(anchor);
    request.chapter_session = chapter_session;
    request.layout.original_epub = draft.original_epub; request.preview = true;
    request.generation = foreground_generation;
    pending_content = std::move(request);
    loading(tr("正在准备排版预览…"));
    clear_body_cache();
    covers.clear();
    trim_glyph_caches_for_free_memory(2U * 1024 * 1024);
}

void ReaderApp::Impl::draw_typography(bool prepare_images) {
    view = View::Typography;
    hits.clear();
    canvas.clear();
    load_fonts(&draft);
    const auto spec = reading_layout(draft, false);
    const bool mode_changed = draft.original_epub != settings.original_epub;
    const bool preview_ready = !mode_changed || typography_preview.has_value();
    const Chapter& source = typography_preview && mode_changed ? typography_preview->chapter : chapter;
    auto start = mode_changed ? typography_preview_offset : offset;
    if (preview_ready && spec.original_epub) start = restore_readable_offset(source, start);
    const bool reuse_body = !mode_changed && body_reusable(start, spec);
    const auto preview = reuse_body ? page : !preview_ready ? graphics::Page{} : plan_page(source, start, spec);
    if (reuse_body) {
        canvas = *body_canvas;
    } else if (!preview_ready) {
        text(typography_preview_failed ? tr("排版预览无法准备") : tr("排版预览尚未就绪"), 36, 96, 28);
    } else if (!prepare_images && !preview.images.empty()) {
        text(tr("预览暂不可用"), 36, 96, 28);
    } else if (async_content && !image_prepared && preview.error.empty() && !preview.images.empty()) {
        begin_image(start, preview, true);
        text(tr("正在准备图片…"), 36, 300, 26, 400, 3);
        button(Dismiss, tr("取消"), 262, 368, 160, 64, false, 24);
        loading_cancel = hits.back();
    } else {
        if (!preview.error.empty() || !draw_page_content(preview, spec, false, &fonts_for(source)))
            text(tr("预览暂不可用"), 36, 96, 28);
    }
    const int panel_top = font_picker_open ? 500 : typography_strip_top();
    for (const auto& line : preview.lines)
        if (line.top < panel_top && line.bottom > panel_top) canvas.fill_rect(0, line.top, 684, panel_top - line.top, 15);
    if (font_picker_open) {
        if (font_fallback || weight_unavailable) {
            int clear_top = 430;
            for (const auto& line : preview.lines)
                if (line.top < clear_top && line.bottom > 430) clear_top = line.top;
            canvas.fill_rect(0, clear_top, 684, 500 - clear_top, 15);
            note(font_fallback ? tr("所选字体不可用，预览使用替代字体。") : tr(kWeightUnavailableNotice), 446);
        }
        draw_font_picker();
        return;
    }
    const bool changed = typography_changed();
    const bool txt = txt_name(book_path);
    canvas.fill_rect(0, panel_top, 684, 1216 - panel_top, 15);
    tabs(typography_tabs_top(), Typography);
    canvas.fill_rect(24, panel_top + 5, 636, 44, 15);
    const int strip_text_top = top_for_middle(ui, 24, panel_top + panel_strip / 2);
    text(changed ? tr("预览 · 尚未应用") : tr("当前排版 · 此书"), 36, strip_text_top, 24, 340, 3);
    constexpr int defaults_left = 420, defaults_width = 96;
    text(tr("恢复默认"), defaults_left, strip_text_top, 24, defaults_width);
    hits.insert(hits.begin(), {defaults_left - 20, panel_top, defaults_width + 40, panel_strip, Defaults});
    text(changed ? tr("放弃修改") : tr("收起"), changed ? 540 : 600, strip_text_top, 24, 120);

    const int style_top = typography_style_top();
    if (txt) {
        panel_choice_card(TxtKeepLineBreaks, tr("保留换行"), panel_card_left, style_top, !draft.txt_reflow);
        panel_choice_card(TxtReflowParagraphs, tr("重排段落"), panel_card_right, style_top, draft.txt_reflow);
    } else {
        panel_choice_card(EpubLayoutOriginal, tr("原书排版"), panel_card_left, style_top, draft.original_epub);
        panel_choice_card(EpubLayoutUnified, tr("统一排版"), panel_card_right, style_top, !draft.original_epub);
    }

    constexpr int card_width = panel_card_width, zone_width = 64;
    const int card_height = typography_card_height;
    const auto card_text = [&](int x, int y, const std::string& label, const std::string& value, int width) {
        text(label, x + 20, top_for_middle(ui, 24, y + 28), 24, width, 3);
        text(value, x + 20, top_for_middle(ui, 28, y + 60), 28, width, 1, true);
    };
    const auto zone_divider = [&](int x, int y) { canvas.fill_rect(x, y, 2, card_height, 1); };
    const auto stepper_card = [&](int x, int y, const std::string& label, const std::string& value,
                                  int less_id, int more_id, bool can_less, bool can_more) {
        outline(x, y, card_width, card_height, 2);
        card_text(x, y, label, value, card_width - 2 * zone_width - 28);
        const int less_x = x + card_width - 2 * zone_width, more_x = x + card_width - zone_width;
        zone_divider(less_x, y);
        zone_divider(more_x, y);
        const int glyph_top = top_for_middle(ui, 40, y + card_height / 2);
        center("−", less_x, glyph_top, zone_width, 40, can_less ? 1 : 5);
        center("+", more_x, glyph_top, zone_width, 40, can_more ? 1 : 5);
        if (can_less) hits.push_back({less_x, y, zone_width, card_height, less_id});
        if (can_more) hits.push_back({more_x, y, zone_width, card_height, more_id});
    };
    const auto toggle_card = [&](int x, int y, int width, int id, const char* label, const char* value) {
        outline(x, y, width, card_height, 2);
        if (width == card_width) {
            const std::string hint = tr("点按切换");
            const int hint_width = text_width(hint, 20);
            card_text(x, y, label, value, width - hint_width - 56);
            text(hint, x + width - 20 - hint_width, top_for_middle(ui, 20, y + card_height / 2), 20, hint_width, 3);
        } else {
            center(label, x + 8, top_for_middle(ui, 24, y + 28), width - 16, 24, 3);
            center(value, x + 8, top_for_middle(ui, 26, y + 60), width - 16, 26, 1, true);
        }
        hits.push_back({x, y, width, card_height, id});
    };
    const bool lightest_weight = draft.font_weight == (light_available(draft) ? "light" : "regular");
    const auto size_card = [&](int x, int y) {
        stepper_card(x, y, tr("字号"), std::to_string(draft.font_size_px) + " px", FontSizeLess, FontSizeMore,
                     draft.font_size_px > 28, draft.font_size_px < 56);
    };
    const auto margin_card = [&](int x, int y) {
        stepper_card(x, y, tr("页边距"), std::to_string(draft.margin_x_px) + " px", MarginLess, MarginMore,
                     draft.margin_x_px > 24, draft.margin_x_px < 52);
    };
    constexpr int left = panel_card_left, right = panel_card_right;
    const int pitch = typography_card_pitch;
    const int row = typography_cards_top();
    panel_navigation_card(FontChoice, tr("字体"), font_name(draft.font), left, row, card_width);
    size_card(right, row);
    const auto fullscreen = draft.fullscreen_reading ? tr("开") : tr("关");
    if (spec.original_epub) {
        margin_card(left, row + pitch);
        toggle_card(right, row + pitch, card_width, FullscreenToggle, tr("全屏阅读"), fullscreen);
        const int adjustment = draft.original_weight_adjustment;
        stepper_card(left, row + 2 * pitch, tr("字重"),
                     adjustment < 0 ? tr("稍细") : adjustment > 0 ? tr("稍粗") : tr("按原书"),
                     OriginalWeightLess, OriginalWeightMore, adjustment > -1, adjustment < 1);
        text(tr("行距、段距和首行缩进按原书"), 36, top_for_middle(ui, 24, 1098), 24, 612, 3);
    } else {
        stepper_card(left, row + pitch, tr("字重"), weight_name(draft.font_weight), FontWeightLess, FontWeightMore,
                     !lightest_weight, draft.font_weight != "bold");
        stepper_card(right, row + pitch, tr("行距"), decimal(draft.line_height), LineHeightLess, LineHeightMore,
                     draft.line_height > 1.35001, draft.line_height < 1.64999);
        margin_card(left, row + 2 * pitch);
        stepper_card(right, row + 2 * pitch, tr("段距"), decimal(draft.paragraph_gap_lines) + tr(" 行"), GapLess, GapMore,
                     draft.paragraph_gap_lines > 0.00001, draft.paragraph_gap_lines < 0.99999);
        stepper_card(left, row + 3 * pitch, tr("首行缩进"), std::to_string(draft.indent_chars) + tr(" 字"), IndentLess, IndentMore,
                     draft.indent_chars > 0, draft.indent_chars < 2);
        constexpr int fullscreen_width = 168, toggle_gap = 12;
        toggle_card(right, row + 3 * pitch, fullscreen_width, FullscreenToggle, tr("全屏阅读"), fullscreen);
        toggle_card(right + fullscreen_width + toggle_gap, row + 3 * pitch,
                    card_width - fullscreen_width - toggle_gap, AlignmentToggle, tr("对齐"),
                    draft.justify ? tr("两端对齐") : tr("左对齐"));
    }

    const int actions_top = typography_actions_top, action_height = typography_action_height;
    if (!font_fallback && !weight_unavailable && !mode_changed && draft.txt_reflow == settings.txt_reflow)
        button(Scope, tr("设为阅读默认"), left, actions_top, card_width, action_height);
    else disabled_button(tr("设为阅读默认"), left, actions_top, card_width, action_height);
    const bool layout_mode_changed = mode_changed || draft.txt_reflow != settings.txt_reflow;
    const bool mode_preview_valid = !layout_mode_changed || (preview_ready && preview.error.empty() && preview.end > preview.start);
    if (font_fallback || weight_unavailable || !mode_preview_valid) disabled_button(tr("无法应用"), right, actions_top, card_width, action_height);
    else if (changed) button(Apply, tr("应用到此书"), right, actions_top, card_width, action_height, true);
    else button(Back, tr("完成"), right, actions_top, card_width, action_height);
    if (preview_ready && spec.original_epub && source.author_style_fallback)
        note(tr("原书样式不可用，本段使用基本排版"), panel_top - 40);
    if (font_fallback || weight_unavailable) {
        canvas.fill_rect(24, panel_top - 70, 636, 58, 15);
        note(weight_unavailable && !font_fallback ? tr(kWeightUnavailableNotice) : tr("所选字体不可用，预览使用替代字体。"), panel_top - 60);
    }
}

void ReaderApp::Impl::draw_reading_settings() {
    view = View::Refresh;
    hits.clear(); canvas.clear(); header(tr("阅读"), Back, tr("设置"));
    const auto &values = edit_snapshot.config.settings;
    text(tr("阅读默认值 · 单本书的调整不受影响"), 36, 118, 24, 612, 5);
    text(tr("排版"), 36, 192, 24, 612, 5);
    canvas.line(36, 234, 647, 234, 12, 2);
    text(tr("默认排版"), 36, 250, 28, 440);
    text(typography_summary(values), 36, 294, 24, 440, 3);
    if (has_book()) {
        text("›", 636, 268, 28, 24);
        hits.push_back({24, 234, 636, 108, Typography});
    } else text(tr("打开书后可调整"), 648 - text_width(tr("打开书后可调整"), 24), 272, 24, 200, 3);
    canvas.line(36, 341, 647, 341, 12, 2);
    text(tr("刷新"), 36, 374, 24, 612, 5);
    const std::array<std::pair<const char*, const char*>, 3> modes{{
        {tr("清晰"), tr("每页执行全刷，减少残影")}, {tr("均衡"), tr("灰阶更新，定期全刷")}, {tr("快速"), tr("优先响应速度，可能更易留痕")}}};
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
    if (values.refresh_preset == "clear") value_line(0, tr("自动全刷间隔"), tr("每页"), 758);
    else value_line(RefreshInterval, tr("自动全刷间隔"), refresh_interval_label(values) + " ›", 758);
    for (const auto& option : page_effect_choices) {
        if (option.effect == page_effect)
            value_line(PageEffects, tr("翻页效果"), std::string(tr(option.title)) + " ›", 848);
    }
    value_line(DictionaryOpen, tr("离线词典"), trf("已启用 {count} 套 ›", {{"count", std::to_string(settings.dictionary_enabled.size())}}), 938);
    button(FullRefresh, tr("立即全刷"), 24, 1080, 636, 96);
}

void ReaderApp::Impl::draw_settings() {
    if (!bookmark_io_ready()) return;
    view = View::Settings;
    hits.clear(); canvas.clear(); header(tr("设置"), Back, tr("书库"));
    const auto cfg = store.load_settings();
    const auto &values = cfg.settings;
    sync::Config account;
    std::string detail;
    const bool configured = sync::load_config(root, account, detail);
    std::uint64_t card_total = 0, card_free = 0;
    const bool measured = store.card_space(card_total, card_free);
    int y = 112;
    const auto row = [&](int id, const std::string& title, const std::string& value) {
        settings_row(id, title, value, y, 12, settings_pitch);
        y += settings_pitch;
    };
    const auto wifi = wifi_summary();
    row(Import, tr("网络与传输"), (wifi.rfind("Wi-Fi", 0) == 0 || wifi.rfind(tr("设备热点"), 0) == 0 ? wifi : "Wi-Fi " + wifi) + " · " +
        (std::time(nullptr) >= provision::clock_floor ? tr("时间已校准") : tr("时间未校准")));
    row(SyncSettings, tr("KOReader 同步"), configured ? server_name(account) + " · " + account.username : tr("未配置"));
    row(LanguageSettings, tr("语言"), i18n::language_name(settings.language));
    row(Refresh, tr("阅读"), trf("{font} {size} · 行距 {spacing} · {refresh}刷新",
        {{"font", font_name(values.font)}, {"size", std::to_string(values.font_size_px)},
         {"spacing", compact(values.line_height)}, {"refresh", refresh_name(values)}}));
    row(InputSettings, tr("书库"), trf("{view}视图 · 文件夹{order}",
        {{"view", values.library_view == "grid" ? tr("卡片") : tr("列表")},
         {"order", values.library_folders == "first" ? tr("置顶") : tr("跟随最近")}}));
    const auto sleep = values.sleep_screen == "reading" ? std::string(tr("休眠保留正文")) : values.sleep_screen == "cover"
        ? std::string(tr("休眠显示封面")) : trf("休眠显示{screen}", {{"screen", sleep_name(values)}});
    row(PowerSettings, tr("电源与休眠"), sleep + " · " +
        (values.auto_sleep_minutes ? trf("{minutes} 分钟后休眠", {{"minutes", std::to_string(values.auto_sleep_minutes)}}) : tr("不自动休眠")));
    row(StorageSettings, tr("存储"), measured ? trf("可用 {size}", {{"size", file_size(card_free)}}) : tr("可用空间未知"));
    const auto minor = [&](int id, const std::string& title, const std::string& value, bool bold, int top) {
        const int value_width = std::min(260, text_width(value, 24));
        text(title, 36, top + 21, 28, value.empty() ? 554 : 628 - value_width - 16 - 36);
        if (!value.empty()) text(value, 628 - value_width, top + 24, 24, value_width, bold ? 1 : 3, bold);
        text("›", 636, top + 20, 28, 24);
        canvas.line(36, top + 81, 647, top + 81, 12, 2);
        hits.push_back({24, top, 636, 82, id});
    };
    const auto offer = update_offer();
    const bool newer = offer == update::Offer::Newer || offer == update::Offer::NewerSchema;
    const auto status = !update_layout_supported ? std::string(tr("需要重装"))
        : update_check.stage != CheckStage::None ? std::string(tr("检查失败"))
        : !update_state.manifest ? std::string(tr("检查更新"))
        : offer == update::Offer::Current || offer == update::Offer::Skipped ? std::string(tr("已是最新"))
        : offer == update::Offer::Development ? std::string(tr("开发构建")) : tr("新版本 ") + update_state.manifest->version;
    minor(UpdateOpen, tr("固件更新"), status, newer && update_check.stage == CheckStage::None, y + 16);
    minor(Help, tr("使用说明"), "", false, y + 98);
    minor(About, tr("关于"), "", false, y + 180);
    if (session.needs_save()) {
        footer_bar(tr("进度尚未保存 · 点此重试"));
        hits.push_back({0, footer_zone, 684, 96, SaveRetry});
    } else footer_bar(cfg.diagnostics.empty() ? notice() : settings_file_problem(cfg.diagnostics) + tr("，请检查后重新打开。"));
}

bool ReaderApp::Impl::save_language(const SettingsSnapshot& snapshot, i18n::Language language, bool onboarding_pending) {
    auto values = snapshot.config.settings;
    values.language = language;
    if (onboarding_pending && !values.onboarding_done) values.onboarding_in_progress = true;
    std::string detail;
    if (store.apply_settings(snapshot, values, false, detail) != StoreStatus::Ok) {
        message = view == View::Onboarding ? tr("语言设置未保存，请检查 TF 卡后重试。") :
            tr("语言设置未保存，请检查 TF 卡后重新打开设置。");
        return false;
    }
    settings.language = values.language;
    settings.onboarding_in_progress = values.onboarding_in_progress;
    for (auto& entry : entries) { entry.described = false; entry.status.clear(); }
    message.clear(); cover_notice.clear();
    return true;
}

void ReaderApp::Impl::draw_language_settings() {
    view = View::LanguageSettings;
    hits.clear(); canvas.clear(); header(tr("语言"), Back, tr("设置"));
    draw_language_choices();
    footer_bar(notice());
}

void ReaderApp::Impl::draw_help_demo() {
    const bool full = help_page == 3;
    if (full) {
        hits.clear();
        hits.push_back({0, 1016, 205, 112, Prev});
        hits.push_back({205, 1016, 274, 112, FullRefresh});
        hits.push_back({479, 1016, 205, 112, Next});
        help_regions(0, 0, 684, 1216);
        const std::array<const char*, 3> names{{tr("上一页"), tr("打开工具"), tr("下一页")}};
        for (int i = 0; i < 3; ++i) {
            const int x = i == 0 ? 0 : i == 1 ? 205 : 479, w = i == 1 ? 274 : 205;
            const auto ink = help_zone == HelpPrevious + i ? 15 : 1;
            center(names[i], x, 370, w, 28, ink, true);
            center(i == 1 ? tr("K2 全刷") : i == 0 ? tr("K1 上一屏") : tr("K3 下一屏"), x, 1024, w, 22, ink);
        }
        canvas.fill_rect(24, 620, 636, 196, 15); outline(24, 620, 636, 196);
        center(tr("点按示例 · 不改变阅读位置"), 36, 646, 612, 26, 1, true);
        center(tr("点任一区域，查看对应操作"), 36, 700, 612, 24);
        center(tr("按 K1 / K3 或左右轻扫离开"), 36, 754, 612, 24);
        const char* positions[] = {tr("第 12 / 340 页"), "3.53%", tr("第 2 / 10 章"), tr("第一章 从一页开始"), ""};
        text(positions[help_position], 36, 1154, 24, 476, help_zone == HelpPosition ? 15 : 1);
        const char* devices[] = {"84%", "12:00", ""};
        center(devices[help_device], 540, 1154, 144, 24, help_zone == HelpDevice ? 15 : 1);
        return;
    }
    help_regions(36, 276, 274, 486);
    const char* labels[] = {tr("左侧：上一页"), tr("中部：打开工具"), tr("右侧：下一页"), tr("页脚左：切换位置"), tr("页脚右：切换状态")};
    for (int i = 0; i < 5; ++i) {
        const int id = HelpPrevious + i, y = 290 + i * 88;
        if (help_zone == id) canvas.fill_rect(330, y, 330, 72, 1);
        text(labels[i], 342, y + 22, 24, 306, help_zone == id ? 15 : 1);
        hits.push_back({330, y, 330, 88, id});
    }
    text(tr("点示意图或说明，查看对应区域"), 36, 786, 24, 612, 3);
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
    const char* description = help_key == HelpKey2 ? tr("所有可交互页面：立即全刷。") :
        help_key == HelpKey1 ? tr("正文：上一页。书库、目录、书签、说明：上一屏。") : tr("正文：下一页。书库、目录、书签、说明：下一屏。");
    wrapped(description, 36, 942, 612, 24);
    title_lines(help_key == HelpKey2 ? tr("休眠准备时忽略 K2，不全刷。") : tr("工具、排版和键盘中不翻动正文。"), 36, 1030, 612, 24, false, 2, 3);
    footer_bar("", help_page, kHelpPages.size());
}

void ReaderApp::Impl::begin_storage(bool clear_cache) {
    abandon_content();
    if (counting_job) counting_job->cancel();
    counting_session.reset(); font_scan.reset();
    clear_prefetch();
    if (clear_cache) {
        release_author_fonts();
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
    loading(clear_cache ? tr("正在清理缓存…") : tr("正在统计存储空间…"));
}

void ReaderApp::Impl::draw_storage() {
    view = View::StorageSettings;
    hits.clear(); canvas.clear(); header(tr("存储"), Back, tr("设置"));
    const auto& roots = storage_usage.roots;
    const auto size = [this](const std::optional<std::uint64_t> &bytes) { return bytes ? file_size(*bytes) : std::string(tr("未知")); };
    text(tr("TF 卡"), 36, 128, 22, 612, 3);
    value_row(0, tr("书籍原文件"), "", size(storage_usage.books), 164, 70);
    value_row(0, tr("缓存（可重建）"), "", size(storage_usage.cache), 234, 70);
    value_row(0, tr("可用空间"), "", size(storage_usage.available), 304, 70, true);
    text(tr("文件位置"), 36, 402, 22, 612, 3);
    value_row(0, tr("书籍与文章"), "", roots.size() == 1 ? roots.front() + "/" : tr("多个目录（见设置文件）"), 438, 70);
    value_row(0, tr("自行添加的字体"), "", "Fonts/", 508, 70);
    value_row(0, tr("设备设置"), "", "PicoPage/settings.toml", 578, 70);
    value_row(0, tr("阅读进度"), "", tr("原书旁的 .picopage/"), 648, 70);
    const std::string notes[] = {notice().empty() ? tr("文章可放扫描目录下的 Articles/；保留 PicoPage/。") : message,
        tr("保留书籍、设置和阅读进度。"), tr("清理后首次开书或查词可能较慢。")};
    int notes_height = 0;
    for (const auto& value : notes) notes_height += std::max(38, wrapped(value, 36, 0, 612, 24, false));
    int note_top = std::min(950, 1064 - notes_height);
    for (const auto& value : notes) note_top += std::max(38, wrapped(value, 36, note_top, 612, 24));
    button(ClearCache, tr("清理全部缓存"), 24, 1080, 636, 96);
}

}
