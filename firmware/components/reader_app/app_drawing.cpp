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
                std::uint8_t gray, bool draw) {
    return title_lines(ui, value, x, y, width, size, bold, max_rows, gray, draw);
}

int ReaderApp::Impl::title_lines(graphics::Font& font, const std::string &value, int x, int y, int width, int size,
                                bool bold, int max_rows, std::uint8_t gray, bool draw) {
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
        int offset = 0;
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
                // Byte span of the part: recorded boundaries for GB18030, 48 KiB steps for
                // UTF-8, both limited by the file size so that a short part is not overstated.
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
            // The number of parts is unknown before the item is counted; one more is assumed.
            const std::uint64_t parts = at.part + (at.last_part ? 1 : 2);
            const std::uint64_t within = at.part * size + end;
            read += spine[i].bytes * within / (parts * size);
        }
    }
    return {read, total};
}


void ReaderApp::Impl::reading_header(bool panel) {
    canvas.fill_rect(0, 0, 684, 104, 15);
    text(panel ? "‹ 正文" : "‹ 书库", 28, 32, 28, 100);
    hits.push_back({0, 0, 128, 104, panel ? Back : Library});
    const bool unsaved = session.needs_save(), differs = remote_anchor.has_value();
    // The tool page adds the bookmark cell at the right end and moves the badge left of it.
    const bool marking = view == View::Tools;
    const int edge = marking ? bookmark_cell_left - 12 : 660;
    const int right = unsaved || differs ? edge - 164 : edge;
    text(body, chapter_label(), 132, 14, 30, right - 132, 1, true);
    const auto percent = " · " + book_percent();
    const int percent_width = text_width(percent, 22);
    const int title_width = std::min(text_width(body, document.metadata().title, 22), right - 132 - percent_width);
    text(body, document.metadata().title, 132, 60, 22, title_width, 3);
    text(percent, 132 + title_width, 60, 22, percent_width, 3);
    if (unsaved || differs) {
        button(Sync, unsaved ? "尚未保存" : "位置不同 ›", edge - 148, 26, 148, 54, true, 22);
        if (view == View::Sync) hits.pop_back();
    }
    if (marking) {
        const bool present = !page_bookmarks().empty();
        canvas.line(bookmark_cell_left, 16, bookmark_cell_left, 87, 12, 2);
        bookmark_ribbon(bookmark_cell_left + 50, 14, 38, present);
        center(present ? "删书签" : "加书签", bookmark_cell_left, 62, 684 - bookmark_cell_left, 20);
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
        // A screen painted solid: the whole panel is redrawn.
        outline(cx - 15, cy - 18, 30, 36, 3);
        canvas.fill_rect(cx - 9, cy - 12, 18, 24, 1);
    }
}


} // namespace pico_page
