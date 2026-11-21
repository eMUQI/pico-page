#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::text(const std::string &value, int x, int top, int size, int max_width,
          std::uint8_t gray, bool bold) {
    text(ui, value, x, top, size, max_width, gray, bold);
}

void ReaderApp::Impl::text(graphics::Font& font, const std::string& value, int x, int top, int size,
          int max_width, std::uint8_t gray, bool bold) {
    const FontSizeRestore restore(font, &font == &body);
    if (!font.set_pixel_size(size))
        return;
    const int baseline = top + font.metrics().ascender;
    const auto scalars = decode_utf8(value);
    std::vector<int> widths;
    int total = 0;
    for (auto scalar : scalars) {
        const int width = font.advance(scalar);
        widths.push_back(width);
        total += width;
    }
    const int ellipsis = font.advance(U'…');
    const bool clipped = total > max_width;
    int used = 0;
    for (std::size_t i = 0; i < scalars.size(); ++i) {
        if (used + widths[i] > (clipped ? max_width - ellipsis : max_width)) {
            font.draw(canvas, U'…', x + used, baseline, gray, bold);
            break;
        }
        font.draw(canvas, scalars[i], x + used, baseline, gray, bold);
        used += widths[i];
    }
}

int ReaderApp::Impl::title_lines(const std::string &value, int x, int y, int width, int size, bool bold, int max_rows,
                std::uint8_t gray, bool draw, bool centered) {
    return title_lines(ui, value, x, y, width, size, bold, max_rows, gray, draw, centered);
}

int ReaderApp::Impl::title_lines(graphics::Font& font, const std::string &value, int x, int y, int width, int size,
                                bool bold, int max_rows, std::uint8_t gray, bool draw, bool centered) {
    const FontSizeRestore restore(font, &font == &body);
    if (!font.set_pixel_size(size)) return 0;
    const auto metrics = font.metrics();
    const int line_height = &font == &ui ? size + 8 : std::max(size + 8, metrics.ascender + metrics.descender);
    const auto scalars = decode_utf8(value);
    const auto closing = [](char32_t c) {
        return std::u32string_view(U"，。！？、：；）】》」』〉’”﹐﹑﹒﹔﹕﹖﹗﹚﹜﹞｡､｣,.!?;:)]}").find(c) != std::u32string_view::npos;
    };
    std::size_t begin = 0;
    int rows = 0;
    for (; begin < scalars.size() && rows < max_rows; ++rows) {
        std::size_t end = begin;
        int used = 0;
        while (end < scalars.size()) {
            const int advance = font.advance(scalars[end]), run = run_width(font, scalars, end);
            if (end > begin && (used + advance > width ||
                (rows + 1 < max_rows && run <= width && used + run > width))) break;
            if (used + advance > width) break;
            used += advance;
            ++end;
        }
        const bool ellipsis = rows + 1 == max_rows && end < scalars.size();
        if (ellipsis) {
            const int mark = font.advance(U'…');
            while (end > begin && used + mark > width) used -= font.advance(scalars[--end]);
        } else if (end < scalars.size()) {
            while (end > begin + 1 && closing(scalars[end])) --end;
        }
        int row_width = ellipsis ? font.advance(U'…') : 0;
        if (centered) for (auto i = begin; i < end; ++i) row_width += font.advance(scalars[i]);
        int offset = centered ? std::max(0, (width - row_width) / 2) : 0;
        const int baseline = y + metrics.ascender + rows * line_height;
        for (auto i = begin; i < end; ++i) {
            if (draw) font.draw(canvas, scalars[i], x + offset, baseline, gray, bold);
            offset += font.advance(scalars[i]);
        }
        if (ellipsis && draw) font.draw(canvas, U'…', x + offset, baseline, gray, bold);
        if (ellipsis || end == begin) { ++rows; break; }
        begin = end;
    }
    return rows ? metrics.ascender + metrics.descender + (rows - 1) * line_height : 0;
}

std::pair<std::uint64_t, std::uint64_t> ReaderApp::Impl::estimated_position(std::size_t index, const sync::PartPosition &at) const {
    const auto &spine = document.spine();
    const std::uint64_t size = at.part_scalars, end = std::min(at.offset, at.part_scalars);
    std::uint64_t read = 0, total = 0;
    for (std::size_t i = 0; i < spine.size(); ++i) {
        if (!spine[i].linear) continue;
        total += spine[i].bytes;
        if (i < index) read += spine[i].bytes;
        else if (i == index && size) {
            if (txt_name(book_path)) {
                const auto &offsets = document.metadata().txt_offsets;
                std::uint64_t begin = static_cast<std::uint64_t>(at.part) * kTxtPartBytes, span = kTxtPartBytes;
                if (at.part + 1 < offsets.size()) {
                    begin = offsets[at.part];
                    span = offsets[at.part + 1] - begin;
                }
                begin = std::min<std::uint64_t>(begin, spine[i].bytes);
                span = std::min<std::uint64_t>(span, spine[i].bytes - begin);
                read += begin + span * end / size;
                continue;
            }
            const std::uint64_t parts = at.part + (at.last_part ? 1 : 2);
            const std::uint64_t within = at.part * size + end;
            read += spine[i].bytes * within / (parts * size);
        }
    }
    return {read, total};
}

void ReaderApp::Impl::reading_header(bool panel) {
    canvas.fill_rect(0, 0, 684, 104, 15);
    const auto back_label = panel ? tr("‹ 正文") : tr("‹ 书库");
    const int chapter_left = 32 + std::max(100, text_width(back_label, 28) + 16);
    text(back_label, 28, 32, 28, chapter_left - 32);
    hits.push_back({0, 0, chapter_left - 4, 104, panel ? Back : Library});
    const bool tools_open = view == View::Tools;
    constexpr int device_column_width = 96;
    constexpr int top_row_center = 33, bottom_row_center = 76;
    const int right = tools_open ? bookmark_cell_left - device_column_width : 660;
    text(body, chapter_label(), chapter_left, top_for_middle(body, 30, top_row_center, true),
         30, right - chapter_left, 1, true);
    const auto progress = tools_open ? trf("已读 {percent}", {{"percent", book_percent()}}) : " · " + book_percent();
    const int progress_width = text_width(progress, 22);
    const int title_gap = tools_open ? 22 : 0;
    const auto title = reading_footer::display_label(document.metadata().title);
    bool use_body_font = !tools_open;
    if (tools_open) {
        for (const auto scalar : decode_utf8(title)) {
            if (ui.missing(scalar) && body.contains(scalar)) {
                use_body_font = true;
                break;
            }
        }
    }
    auto& title_font = use_body_font ? body : ui;
    const int title_width = std::clamp(right - chapter_left - progress_width - title_gap,
                                       0, text_width(title_font, title, 22));
    if (title_width > 0)
        text(title_font, title, chapter_left, top_for_middle(title_font, 22, bottom_row_center),
             22, title_width, 3);
    text(progress, chapter_left + (title_width ? title_width + title_gap : 0),
         top_for_middle(ui, 22, bottom_row_center), 22, progress_width, 3);
    if (tools_open) {
        const int column_left = bookmark_cell_left - device_column_width;
        center(reading_footer::clock(std::time(nullptr), 8 * 60), column_left, top_for_middle(ui, 22, top_row_center),
               device_column_width, 22, 3);
        draw_battery_icon(column_left + device_column_width / 2, bottom_row_center);
        const bool present = !page_bookmarks().empty();
        canvas.line(bookmark_cell_left, 16, bookmark_cell_left, 87, 12, 2);
        bookmark_ribbon(bookmark_cell_left + 50, 14, 38, present);
        center(present ? tr("删书签") : tr("加书签"), bookmark_cell_left, 62, 684 - bookmark_cell_left, 20);
        hits.push_back({bookmark_cell_left, 0, 684 - bookmark_cell_left, 104, BookmarkToggle});
    }
    canvas.line(0, 103, 683, 103, 1, 3);
}

void ReaderApp::Impl::tool_icon(int id, int cx, int cy) {
    if (id == Contents) {
        for (int y : {-9, 0, 9}) canvas.line(cx - 15, cy + y, cx + 15, cy + y, 1, 3);
    } else if (id == Typography) {
        center("Aa", cx - 30, cy - 26, 60, 34);
    } else if (id == Sync) {
        for (int d : {-1, 1}) {
            const int x = cx + d * 9;
            canvas.line(x, cy - 18, x, cy + 18, 1, 2);
            canvas.line(x, cy + d * 18, x - 6, cy + d * 10, 1, 2);
            canvas.line(x, cy + d * 18, x + 6, cy + d * 10, 1, 2);
        }
    } else if (id == Refresh) {
        for (int y = -18; y <= 18; ++y)
            for (int x = -18; x <= 18; ++x) {
                const int radius = x*x + y*y;
                if (radius <= 324 && (x <= 0 || radius >= 256)) canvas.pixel(cx+x, cy+y, 1);
            }
    } else {
        outline(cx - 15, cy - 18, 30, 36, 3);
        canvas.fill_rect(cx - 9, cy - 12, 18, 24, 1);
    }
}

}
