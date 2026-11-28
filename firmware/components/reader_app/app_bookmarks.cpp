#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::toggle_bookmark() {
    const auto &visible = session.visible_record();
    if (view != View::Tools || !has_book() || !visible || bookmark_job) return;
    if (!bookmarks_usable() && bookmarks.status != StoreStatus::IdentityMismatch) {
        message = bookmarks.status == StoreStatus::UnsupportedVersion
                      ? tr("书签文件版本较新，无法修改；原文件未修改")
                      : tr("书签文件无法读取，暂不能修改；原文件未修改");
        draw_tools(); overlay(); return;
    }
    ContentJob::Request request;
    request.kind = ContentJob::Kind::SaveBookmarks;
    request.root = root; request.relative_path = book_path; request.identity = document.metadata();
    request.content_revision = revision_for(book_path, document.metadata());
    request.bookmark_base = bookmarks;
    request.bookmarks = bookmarks_usable() ? bookmarks.bookmarks : std::vector<Bookmark>{};
    request.bookmark_next = bookmarks_usable() ? bookmarks.next_sequence : 1;
    const auto present = page_bookmarks();
    bookmark_adding = present.empty();
    auto &list = request.bookmarks;
    if (bookmark_adding) {
        if (list.size() >= kMaxBookmarks) {
            message = tr("书签已达 ") + std::to_string(kMaxBookmarks) + tr(" 个，请先删除不需要的书签");
            draw_tools(); overlay(); return;
        }
        Bookmark bookmark;
        bookmark.sequence = request.bookmark_next++;
        bookmark.anchor = visible->anchor;
        bookmark.chapter = snippet(decode_utf8(chapter_label()), 40);
        const auto start = std::min(page.start, chapter.text.size());
        bookmark.excerpt = snippet(std::u32string_view(chapter.text).substr(start, std::min<std::size_t>(page.end, chapter.text.size()) - start), 40);
        bookmark.percent = book_percent_value(page.start, page.end);
        list.push_back(std::move(bookmark));
    } else {
        for (auto i = present.rbegin(); i != present.rend(); ++i) list.erase(list.begin() + static_cast<std::ptrdiff_t>(*i));
    }
    bookmark_job = std::make_unique<ContentJob>(std::move(request));
    if (!async_content) {
        auto &job = *bookmark_job;
        FileStore direct(root);
        job.result.bookmark_status = direct.save_bookmarks(book_path, job.request.identity.book_id, job.request.content_revision,
                                                           *job.request.bookmark_base, job.request.bookmarks, job.request.bookmark_next,
                                                           job.result.bookmarks);
        if (job.result.bookmark_status == StoreStatus::ExternalChange)
            job.result.bookmarks = direct.load_bookmarks(book_path, job.request.identity.book_id, job.request.content_revision);
        finish_bookmark(true);
        return;
    }
    if (counting_job) counting_job->cancel();
    message = bookmark_adding ? tr("正在保存书签…") : tr("正在删除书签…");
    draw_tools(); overlay();
    poll_bookmark();
}

bool ReaderApp::Impl::finish_bookmark(bool redraw) {
    auto job = std::move(bookmark_job);
    const auto status = job->result.bookmark_status;
    if (job->request.relative_path != book_path || job->request.identity.book_id != document.metadata().book_id)
        return status == StoreStatus::Ok;
    if (status == StoreStatus::Ok) bookmark_drain_failed = false;
    if (status == StoreStatus::Ok || status == StoreStatus::ExternalChange) {
        bookmarks = std::move(job->result.bookmarks);
        ++bookmark_version;
    }
    if (status == StoreStatus::Ok) message = bookmark_adding ? tr("已加书签") : tr("已删除本页书签");
    else if (status == StoreStatus::ExternalChange) message = tr("书签文件已在别处修改，已重新读取，请核对当前书签");
    else message = bookmark_adding ? tr("书签未保存，请检查 TF 卡后重试") : tr("书签未删除，请检查 TF 卡后重试");
    if (redraw) {
        if (view == View::Tools) { draw_tools(); overlay(); }
        else if (view == View::Contents && status == StoreStatus::Ok) { draw_contents(); overlay(); }
        else if (view == View::Reading && !image_viewing && render_body(offset)) overlay();
        else if (status != StoreStatus::Ok) bookmark_error = message;
    }
    return status == StoreStatus::Ok;
}

void ReaderApp::Impl::draw_toolbar() {
    canvas.fill_rect(0, toolbar_top, 684, 136, 15);
    canvas.line(0, toolbar_top, 683, toolbar_top, 1, 3);
    for (int i = 0; i < 5; ++i) {
        const int x = i * 137, width = std::min(137, 684 - x);
        if (i) canvas.line(x, toolbar_top, x, 1215, 1, 2);
        tool_icon(kToolbarIds[i], x + 68, 1126);
        const auto label = kToolbarIds[i] == Refresh && text_width(tr(kToolbarLabels[i]), 28) > width - 16
            ? tr("模式") : tr(kToolbarLabels[i]);
        center(label, x, 1160, width, 28);
        hits.push_back({x, toolbar_top, width, 136, kToolbarIds[i]});
    }
}

void ReaderApp::Impl::draw_tools() {
    typography_preview.reset();
    tools_save_pending = session.needs_save();
    view = View::Tools;
    hits.clear();
    load_fonts();
    if (!render_body(offset)) return;
    for (const auto &line : page.lines)
        if ((line.top < 104 && line.bottom > 104) || (line.top < toolbar_top && line.bottom > toolbar_top))
            canvas.fill_rect(0, line.top, 684, line.bottom - line.top + 1, 15);
    reading_header();
    draw_toolbar();
    if (!notice().empty()) {
        canvas.fill_rect(24, 1012, 636, 56, 15);
        note(message, 1024);
    }
    hits.push_back({0, 104, 684, toolbar_top - 104, Dismiss});
}

void ReaderApp::Impl::draw_bookmarks(int list_top) {
    constexpr int row = bookmark_row;
    constexpr auto per_page = bookmarks_per_screen;
    const auto &items = bookmarks.bookmarks;
    const auto problem = bookmark_problem();
    if (!problem.empty() || items.empty()) {
        wrapped(problem.empty() ? tr("还没有书签。在正文工具页点右上角的「加书签」，可标记当前页。") : problem,
                36, list_top + 40, 612, 24, true, 3);
        footer_bar("");
        return;
    }
    const auto pages = (items.size() + per_page - 1) / per_page;
    bookmark_page = std::min(bookmark_page, pages - 1);
    const auto present = page_bookmarks();
    int y = list_top;
    for (auto i = bookmark_page * per_page; i < std::min(items.size(), (bookmark_page + 1) * per_page); ++i) {
        const auto &item = items[i];
        const bool selected = std::find(present.begin(), present.end(), i) != present.end();
        const std::uint8_t ink = selected ? 15 : 1;
        if (selected) canvas.fill_rect(0, y, 684, row - 1, 1);
        const auto position = selected ? std::string(tr("本页")) : item.percent >= 0 ? std::to_string(item.percent) + "%" : std::string();
        const int width = text_width(position, 22);
        title_lines(item.chapter.empty() ? ui : body, item.chapter.empty() ? tr("书签 ") + std::to_string(item.sequence) : item.chapter,
                    36, y + 18, 600 - width - 24, 26, selected, 1, ink);
        text(position, 648 - width, y + 20, 22, width, selected ? 15 : 3);
        title_lines(item.excerpt.empty() ? ui : body, item.excerpt.empty() ? tr("（本页没有文字）") : item.excerpt, 36, y + 64, 612, 22, false, 1, selected ? 15 : 3);
        if (!selected) canvas.line(0, y + row - 1, 683, y + row - 1, 1, 2);
        hits.push_back({0, y, 684, row, BookmarkBase + static_cast<int>(i)});
        y += row;
    }
    footer_bar("", bookmark_page, pages);
}

void ReaderApp::Impl::contents_switch(int top) {
    constexpr int label_size = 24, padding = 24, underline_height = 3, underline_gap = 10, minimum_hit_width = 160;
    const int bottom = top + contents_switch_height, middle = top + (contents_switch_height - underline_gap) / 2;
    int jump_left = 684;
    if (!contents_bookmarks) {
        const std::string label = tr("快速跳转 ›");
        jump_left = 684 - text_width(label, label_size) - 2 * padding;
        canvas.fill_rect(jump_left, top, 2, contents_switch_height, 1);
        center(label, jump_left + 2, top_for_middle(ui, label_size, middle),
               684 - jump_left - 2, label_size, 1);
        hits.push_back({jump_left, top, 684 - jump_left, contents_switch_height, ContentsJumpOpen});
    }
    const std::string names[] = {tr("目录"), bookmarks_usable() && !bookmarks.bookmarks.empty()
        ? trf("书签 {count}", {{"count", std::to_string(bookmarks.bookmarks.size())}}) : tr("书签")};
    const int actions[] = {ContentsToc, ContentsBookmarks};
    int hit_left = 0, label_left = 36;
    for (int index = 0; index < 2; ++index) {
        const bool active = (index == 1) == contents_bookmarks;
        const int label_width = std::min(text_width(names[index], label_size), jump_left - padding - label_left);
        text(names[index], label_left, top_for_middle(ui, label_size, middle, active), label_size, label_width,
             active ? 1 : 3, active);
        if (active)
            canvas.fill_rect(label_left, bottom - 2 - underline_gap - underline_height, label_width, underline_height, 1);
        const int label_right = label_left + label_width + padding;
        const int hit_right = index ? std::min(jump_left, std::max(label_right, hit_left + minimum_hit_width)) : label_right;
        hits.push_back({hit_left, top, hit_right - hit_left, contents_switch_height, actions[index]});
        if (!index) canvas.fill_rect(hit_right, middle - 19, 2, 38, 12);
        hit_left = hit_right + 2;
        label_left = hit_left + padding;
    }
    canvas.fill_rect(0, bottom - 2, 684, 2, 1);
}

void ReaderApp::Impl::draw_contents_jump(int top) {
    hits.erase(std::remove_if(hits.begin(), hits.end(), [top](const Hit& hit) { return hit.y >= top; }), hits.end());
    canvas.fill_rect(0, top, 684, 1216 - top, 15);
    const auto preview = contents_jump_preview();
    const auto count = contents_jump_toc_count();
    const auto current_percent = std::to_string(preview.current_percent) + "%";
    const std::string title = tr("快速跳转"), back_label = tr("‹ 目录");
    const int back_width = text_width(back_label, 24);
    const int divider = 36 + back_width + 24;
    const int title_left = divider + 26;
    const int bar_middle = top + 43;
    text(back_label, 36, top_for_middle(ui, 24, bar_middle), 24, back_width);
    canvas.fill_rect(divider, top + 27, 2, 34, 12);
    if (count) {
        const auto status = trf("共 {count} 项", {{"count", std::to_string(count)}});
        const int status_width = text_width(status, 24), status_left = 648 - status_width;
        text(title, title_left, top_for_middle(ui, 28, bar_middle, true), 28, status_left - title_left - 16, 1, true);
        text(status, status_left, top_for_middle(ui, 24, bar_middle), 24, status_width, 3);
    } else text(title, title_left, top_for_middle(ui, 28, bar_middle, true), 28, 648 - title_left, 1, true);
    const int header_bottom = top + contents_switch_height;
    hits.push_back({0, top, divider, contents_switch_height, ContentsJumpBack});
    canvas.fill_rect(0, header_bottom - 2, 684, 2, 1);

    constexpr int left = 24, width = 636, card_width = 312, second_card = 348;
    constexpr int mode_top = 548, mode_height = 72, card_top = 630;
    if (count) {
        const auto mode_card = [&](int action, const char* label, int x, bool selected) {
            panel_choice_card(action, label, x, mode_top, selected);
            hits.back().y = header_bottom;
            hits.back().h = card_top - header_bottom;
        };
        mode_card(ContentsJumpChapter, tr("按目录序号"), panel_card_left, !contents_jump_percent);
        mode_card(ContentsJumpPercent, tr("按百分比"), panel_card_right, contents_jump_percent);
    } else
        text(tr("本书未提供目录，将按全书进度跳转"), 36, top_for_middle(ui, 24, mode_top + mode_height / 2), 24, 612, 3);

    constexpr int input_height = 88, preview_height = 80, card_height = input_height + preview_height;
    constexpr int step_width = 88, input_middle = card_top + input_height / 2, preview_top = card_top + input_height;
    constexpr int step_left = left + width - 2 * step_width, inset = 20, readout_left = left + inset;
    constexpr int preview_width = width - 2 * inset;
    const bool has_input = !contents_jump_input.empty();
    const std::string unit = contents_jump_percent ? "%" : "";
    const auto minimum = std::to_string(preview.minimum) + unit, maximum = std::to_string(preview.maximum) + unit;
    const auto readout = has_input ? contents_jump_input + unit
                                   : trf("输入 {min} – {max}", {{"min", minimum}, {"max", maximum}});
    const int readout_size = has_input ? 34 : 28;
    const int readout_width = std::min(text_width(readout, readout_size), step_left - readout_left - 48);
    text(readout, readout_left, top_for_middle(ui, readout_size, input_middle, has_input),
         readout_size, readout_width, has_input ? 1 : 3, has_input);
    text("|", readout_left + readout_width + 4, top_for_middle(ui, 32, input_middle), 32, 24, 5);
    const auto step = [&](int action, const char* label, int x, bool enabled) {
        canvas.fill_rect(x, card_top, 2, input_height, 1);
        if (enabled) hits.push_back({x, card_top, step_width, input_height, action});
        center(label, x, top_for_middle(ui, 40, input_middle), step_width, 40, enabled ? 1 : 5);
    };
    step(ContentsJumpMinus, "−", step_left, preview.can_step_down);
    step(ContentsJumpPlus, "+", step_left + step_width, preview.can_step_up);
    canvas.fill_rect(left, preview_top, width, 2, 1);
    outline(left, card_top, width, card_height, preview.out_of_range ? 3 : 2);

    constexpr int preview_middle = preview_top + 24, detail_middle = preview_top + 58;
    std::string detail;
    if (preview.out_of_range) {
        constexpr int badge = 28;
        canvas.fill_rect(readout_left, preview_middle - badge / 2, badge, badge, 1);
        center("!", readout_left, top_for_middle(ui, 20, preview_middle, true), badge, 20, 15, true);
        const auto error = trf("超出范围，请输入 {min} – {max}", {{"min", minimum}, {"max", maximum}});
        text(error, readout_left + badge + 12, top_for_middle(ui, 28, preview_middle, true),
             28, preview_width - badge - 12, 1, true);
        detail = tr("点按「退格」修改，或点按「清空」重新输入");
    } else if (!has_input) {
        const auto current_label = tr("当前 · ");
        const int label_width = text_width(current_label, 28);
        text(current_label, readout_left, top_for_middle(ui, 28, preview_middle), 28, label_width, 3);
        auto& font = !count && txt_name(book_path) ? ui : body;
        const int label_left = readout_left + label_width;
        text(font, chapter_label(), label_left, top_for_middle(font, 28, preview_middle), 28,
             preview_width - label_width);
        detail = preview.current_toc
            ? trf("第 {number} 项 · {percent}", {{"number", std::to_string(*preview.current_toc + 1)}, {"percent", current_percent}})
            : trf("已读 {percent}", {{"percent", current_percent}});
    } else if (preview.target_toc) {
        const auto target = *preview.target_toc;
        const auto ordinal = target + 1;
        const auto current = preview.current_toc ? *preview.current_toc + 1 : 0;
        std::string distance;
        if (ordinal == current) distance = tr("当前所在章节");
        else if (ordinal > current) distance = trf("向后 {count} 项", {{"count", std::to_string(ordinal - current)}});
        else distance = trf("向前 {count} 项", {{"count", std::to_string(current - ordinal)}});
        text(body, document.toc()[target].label, readout_left, top_for_middle(body, 28, preview_middle, true),
             28, preview_width, 1, true);
        const auto number = std::to_string(ordinal);
        detail = preview.target_percent < 0
            ? trf("第 {number} 项 · {percent}", {{"number", number}, {"percent", distance}})
            : trf("第 {number} 项 · 约 {percent} · {distance}",
                  {{"number", number}, {"percent", std::to_string(preview.target_percent) + "%"}, {"distance", distance}});
    } else if (preview.target_position) {
        const auto target = trf("全书约 {percent} 处", {{"percent", std::to_string(preview.target_percent) + "%"}});
        text(target, readout_left, top_for_middle(ui, 28, preview_middle, true), 28, preview_width, 1, true);
        detail = tr("当前 · ") + trf("已读 {percent}", {{"percent", current_percent}});
    } else detail = tr("目标位置不在正文中");
    text(detail, readout_left, top_for_middle(ui, 24, detail_middle), 24, preview_width, 3);

    constexpr int pad_top = 812, key_width = 204, key_height = 64, key_dx = 216, key_dy = 72;
    constexpr int action_top = 1108, action_height = 96;
    constexpr int hit_top = (card_top + card_height + pad_top) / 2;
    constexpr int hit_bottom = (pad_top + 3 * key_dy + key_height + action_top) / 2;
    constexpr int keys[] = {ContentsJumpDigit0 + 1, ContentsJumpDigit0 + 2, ContentsJumpDigit0 + 3,
                            ContentsJumpDigit0 + 4, ContentsJumpDigit0 + 5, ContentsJumpDigit0 + 6,
                            ContentsJumpDigit0 + 7, ContentsJumpDigit0 + 8, ContentsJumpDigit0 + 9,
                            ContentsJumpClear, ContentsJumpDigit0, ContentsJumpBackspace};
    for (int index = 0; index < 12; ++index) {
        const int row = index / 3, col = index % 3;
        const int x = left + col * key_dx, y = pad_top + row * key_dy, action = keys[index];
        const bool digit = action >= ContentsJumpDigit0 && action <= ContentsJumpDigit9;
        const auto label = digit ? std::to_string(action - ContentsJumpDigit0)
                                 : action == ContentsJumpClear ? tr("清空") : tr("退格");
        if (!digit && !has_input) {
            disabled_button(label, x, y, key_width, key_height);
            continue;
        }
        outline(x, y, key_width, key_height);
        button_label(label, x, y, key_width, key_height, digit ? 34 : 28, 1);
        const int hit_y = row ? y - (key_dy - key_height) / 2 : hit_top;
        const int bottom = row < 3 ? y + key_height + (key_dy - key_height) / 2 : hit_bottom;
        const int half_gap = (key_dx - key_width) / 2;
        hits.push_back({x - half_gap, hit_y, key_dx, bottom - hit_y, action});
    }
    int jump_left = left, jump_width = width;
    if (count) {
        jump_left = second_card;
        jump_width = card_width;
        if (preview.target_toc)
            button(ContentsJumpLocate, tr("在目录中查看"), left, action_top, card_width, action_height);
        else disabled_button(tr("在目录中查看"), left, action_top, card_width, action_height);
    }
    const auto jump = preview.same_position ? tr("已在当前位置")
                        : contents_jump_percent ? tr("跳到此位置") : tr("跳到此章");
    if (preview.can_jump)
        button(ContentsJumpGo, jump, jump_left, action_top, jump_width, action_height, true);
    else disabled_button(jump, jump_left, action_top, jump_width, action_height);
}

void ReaderApp::Impl::draw_contents() {
    view = View::Contents;
    if (!panel_background(contents_top - panel_strip)) return;
    tabs(contents_top, Contents);
    if (contents_jump_open) { draw_contents_jump(contents_top + 100); return; }
    contents_switch(contents_top + 100);
    const int list_top = contents_top + 100 + contents_switch_height, list_height = footer_zone - list_top;
    if (contents_bookmarks) { draw_bookmarks(list_top); return; }
    const auto &toc = document.toc();
    toc_visible.clear();
    toc_starts = {0};
    if (!contents_jump_toc_count()) {
        title_lines(tr("本书未提供目录。可点按右上角「快速跳转」按全书进度定位，或收起面板继续阅读。"),
                    36, list_top + 40, 612, 24, false, 2, 3);
        toc_page = 0;
        toc_locate_current = false;
        return;
    }
    const auto current = current_toc();
    const auto focus = contents_jump_located.value_or(toc_locate_current ? current : toc.size());
    const auto target_label = !contents_jump_located ? std::string{} : contents_jump_located_percent < 0 ? tr("目标")
        : trf("目标 · {percent}", {{"percent", std::to_string(contents_jump_located_percent) + "%"}});
    auto title_width = [&](std::size_t index) {
        int width = 456;
        if (index != current && contents_jump_located == index)
            width = std::min(width, 596 - text_width(target_label, 22));
        return width - std::min<int>(toc[index].depth, 3) * 40 - (toc_parent(index) ? 88 : 0);
    };
    auto title_height = [&](std::size_t index) {
        return title_lines(body, toc[index].label, 0, 0, title_width(index), toc[index].depth ? 24 : 28,
                           false, 2, 1, false);
    };
    const int minimum_row = list_height / 6;
    auto row_height = [&](std::size_t index) {
        return std::max(minimum_row, title_height(index) + 28);
    };
    int used = 0;
    for (std::size_t i = 0; i < toc.size(); ++i) {
        const int height = row_height(i);
        if (used && used + height > list_height) { toc_starts.push_back(toc_visible.size()); used = 0; }
        if (i == focus) toc_page = toc_starts.size() - 1;
        toc_visible.push_back(i);
        used += height;
        if (toc_collapsed.count(i)) {
            const auto depth = toc[i].depth;
            while (i + 1 < toc.size() && toc[i+1].depth > depth) ++i;
        }
    }
    toc_locate_current = false;
    toc_page = std::min(toc_page, toc_starts.size() - 1);
    const auto begin = toc_starts[toc_page];
    const auto end = toc_page + 1 < toc_starts.size() ? toc_starts[toc_page+1] : toc_visible.size();
    int natural_height = 0;
    for (auto row = begin; row < end; ++row) natural_height += row_height(toc_visible[row]);
    const int spare = list_height - natural_height, rows = static_cast<int>(end - begin);
    const int stretch = spare > 0 && spare < minimum_row ? spare : 0;
    const bool paged = toc_starts.size() > 1;
    int y = list_top;
    for (std::size_t row = begin; row < end; ++row) {
        const auto i = toc_visible[row];
        const auto &item = toc[i];
        const int position_in_screen = static_cast<int>(row - begin);
        const int h = row_height(i) + stretch / rows + (position_in_screen < stretch % rows ? 1 : 0);
        const int indent = std::min<int>(item.depth, 3) * 40;
        const bool selected = i == current, parent = toc_parent(i), located = !selected && contents_jump_located == i;
        const std::uint8_t ink = selected ? 15 : 1;
        if (selected) canvas.fill_rect(0, y, 684, h - 1, 1);
        if (located) canvas.fill_rect(0, y, 8, h - 1, 1);
        title_lines(body, item.label, 36 + indent, y + (h - title_height(i)) / 2, title_width(i),
                    item.depth ? 24 : 28, selected, 2, ink);
        const int percent = toc_percent(i);
        const auto position = selected ? tr("当前 · ") + book_percent() : located ? target_label :
                              percent < 0 ? std::string() : std::to_string(percent) + "%";
        const int right = parent ? 560 : 648, width = text_width(position, 22);
        const auto position_metrics = ui.metrics();
        text(position, right - width, y + (h - position_metrics.ascender - position_metrics.descender) / 2,
             22, width, selected ? 15 : located ? 1 : 3);
        if (parent) {
            std::size_t count = 0;
            for (auto j = i + 1; j < toc.size() && toc[j].depth > item.depth; ++j) ++count;
            canvas.line(572, y, 572, y + h - 2, selected ? 15 : 12, 2);
            const int cx = 590, cy = y + h / 2;
            if (toc_collapsed.count(i)) {
                canvas.line(cx - 3, cy - 6, cx + 3, cy, ink, 3);
                canvas.line(cx + 3, cy, cx - 3, cy + 6, ink, 3);
            } else {
                canvas.line(cx - 6, cy - 3, cx, cy + 3, ink, 3);
                canvas.line(cx, cy + 3, cx + 6, cy - 3, ink, 3);
            }
            center(std::to_string(count), 604, y + h / 2 - 20, 52, 26, ink);
            hits.push_back({572, y, 112, h, TocToggleBase + static_cast<int>(i)});
        }
        if (!selected && !(paged && y + h == footer_zone)) canvas.line(0, y + h - 1, 683, y + h - 1, 1, 2);
        hits.push_back({0, y, parent ? 572 : 684, h, TocBase + static_cast<int>(i)});
        y += h;
    }
    footer_bar("", toc_page, toc_starts.size());
}

}
