#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::toggle_bookmark() {
    const auto &visible = session.visible_record();
    if (view != View::Tools || !has_book() || !visible || bookmark_job) return;
    if (!bookmarks_usable() && bookmarks.status != StoreStatus::IdentityMismatch) {
        message = bookmarks.status == StoreStatus::UnsupportedVersion
                      ? "书签文件版本较新，无法修改；原文件未修改"
                      : "书签文件无法读取，暂不能修改；原文件未修改";
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
            message = "书签已达 " + std::to_string(kMaxBookmarks) + " 个，请先删除不需要的书签";
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
    message = bookmark_adding ? "正在保存书签…" : "正在删除书签…";
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
    if (status == StoreStatus::Ok) message = bookmark_adding ? "已加书签" : "已删除本页书签";
    else if (status == StoreStatus::ExternalChange) message = "书签文件已在别处修改，已重新读取，请核对当前书签";
    else message = bookmark_adding ? "书签未保存，请检查 TF 卡后重试" : "书签未删除，请检查 TF 卡后重试";
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
        center(kToolbarLabels[i], x, 1160, width, 28);
        hits.push_back({x, toolbar_top, width, 136, kToolbarIds[i]});
    }
}

void ReaderApp::Impl::draw_tools() {
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
        wrapped(problem.empty() ? "还没有书签。在正文工具页点右上角的「加书签」，可标记当前页。" : problem,
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
        const auto position = selected ? std::string("本页") : item.percent >= 0 ? std::to_string(item.percent) + "%" : std::string();
        const int width = text_width(position, 22);
        title_lines(item.chapter.empty() ? ui : body, item.chapter.empty() ? "书签 " + std::to_string(item.sequence) : item.chapter,
                    36, y + 18, 600 - width - 24, 26, selected, 1, ink);
        text(position, 648 - width, y + 20, 22, width, selected ? 15 : 3);
        title_lines(item.excerpt.empty() ? ui : body, item.excerpt.empty() ? "（本页没有文字）" : item.excerpt, 36, y + 64, 612, 22, false, 1, selected ? 15 : 3);
        if (!selected) canvas.line(0, y + row - 1, 683, y + row - 1, 1, 2);
        hits.push_back({0, y, 684, row, BookmarkBase + static_cast<int>(i)});
        y += row;
    }
    footer_bar("", bookmark_page, pages);
}

void ReaderApp::Impl::draw_contents() {
    view = View::Contents;
    if (!panel_background(contents_top - panel_strip)) return;
    tabs(contents_top, Contents);
    contents_switch(contents_top + 100);
    const int list_top = contents_top + 100 + contents_switch_height, list_height = footer_zone - list_top;
    if (contents_bookmarks) { draw_bookmarks(list_top); return; }
    const auto &toc = document.toc();
    const auto current = current_toc();
    toc_visible.clear();
    toc_starts = {0};
    auto title_width = [&](std::size_t index) {
        return 456 - std::min<int>(toc[index].depth, 3) * 40 - (toc_parent(index) ? 88 : 0);
    };
    auto title_height = [&](std::size_t index) {
        return title_lines(body, toc[index].label, 0, 0, title_width(index), toc[index].depth ? 24 : 28,
                           false, 2, 1, false);
    };
    auto row_height = [&](std::size_t index) {
        return std::max(92, title_height(index) + 28);
    };
    int used = 0;
    for (std::size_t i = 0; i < toc.size(); ++i) {
        const int height = row_height(i);
        if (used && used + height > list_height) { toc_starts.push_back(toc_visible.size()); used = 0; }
        if (toc_locate_current && i == current) toc_page = toc_starts.size() - 1;
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
    int y = list_top;
    for (std::size_t row = begin; row < end; ++row) {
        const auto i = toc_visible[row];
        const auto &item = toc[i];
        const int h = row_height(i), indent = std::min<int>(item.depth, 3) * 40;
        const bool selected = i == current, parent = toc_parent(i);
        const std::uint8_t ink = selected ? 15 : 1;
        if (selected) canvas.fill_rect(0, y, 684, h - 1, 1);
        title_lines(body, item.label, 36 + indent, y + (h - title_height(i)) / 2, title_width(i), item.depth ? 24 : 28, selected, 2, ink);
        const int percent = toc_percent(i);
        const auto position = selected ? "当前 · " + book_percent() : percent < 0 ? std::string() : std::to_string(percent) + "%";
        const int right = parent ? 560 : 648, width = text_width(position, 22);
        const auto position_metrics = ui.metrics();
        text(position, right - width, y + (h - position_metrics.ascender - position_metrics.descender) / 2,
             22, width, selected ? 15 : 3);
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
        if (!selected) canvas.line(0, y + h - 1, 683, y + h - 1, 1, 2);
        hits.push_back({0, y, parent ? 572 : 684, h, TocBase + static_cast<int>(i)});
        y += h;
    }
    if (toc.empty()) note("这本书未提供目录，可收起后继续阅读。", list_top + 40);
    footer_bar("", toc_page, toc_starts.size());
}

}
