#include "pico_page/app.h"
#include "pico_page/document.h"
#include "pico_page/store.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <map>
#include <sys/stat.h>
#include <utility>

namespace pico_page {
namespace {
using graphics::decode_utf8;
constexpr int footer_top = 1128, toolbar_top = 1080;
constexpr const char *content_revision = "epub-native-v1";
// Body updates between automatic full refreshes. The fast preset uses the shorter
// 8-level waveform, whose ghosting is cleared by the more frequent full refresh.
constexpr unsigned balanced_full_refresh_pages = 8, fast_full_refresh_pages = 5;
struct Entry {
    std::string path, name;
    bool directory;
};
struct Hit {
    int x, y, w, h, id;
    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};
enum Control {
    Back = 1,
    Library,
    Tools,
    Contents,
    Typography,
    Refresh,
    FullRefresh,
    Settings,
    Import,
    Sync,
    Prev,
    Next,
    FontSize,
    LineHeight,
    Margin,
    Minus,
    Plus,
    Apply,
    Scope,
    FontChoice,
    Advanced,
    Gap,
    Indent,
    Defaults,
    RefreshClear,
    RefreshBalanced,
    RefreshFast,
    SaveRetry,
    Dismiss,
    LinkReturn,
    SyncCheck,
    SyncJump,
    SyncUpload,
    SyncConfirm,
    EntryBase = 100,
    TocBase = 1000,
    LinkBase = 20000
};
std::string basename(const std::string &path) {
    const auto i = path.find_last_of('/');
    return path.substr(i == std::string::npos ? 0 : i + 1);
}
bool relative_path(const std::string &value) {
    if (value.empty() || value.front() == '/' || value.find('\\') != std::string::npos ||
        value.find('\0') != std::string::npos)
        return false;
    std::size_t p = 0;
    while (p < value.size()) {
        const auto e = value.find('/', p);
        const auto s = value.substr(p, e == std::string::npos ? e : e - p);
        if (s.empty() || s == "." || s == "..")
            return false;
        if (e == std::string::npos)
            return true;
        p = e + 1;
    }
    return false;
}
bool safe_existing(const std::string &root, const std::string &relative) {
    if (!relative_path(relative))
        return false;
    std::size_t end = 0;
    do {
        end = relative.find('/', end);
        const auto prefix = relative.substr(0, end);
        struct stat st{};
#ifdef ESP_PLATFORM
        if (stat((root + "/" + prefix).c_str(), &st) != 0)
            return false;
#else
        if (lstat((root + "/" + prefix).c_str(), &st) != 0 || S_ISLNK(st.st_mode))
            return false;
#endif
        if (end != std::string::npos)
            ++end;
    } while (end != std::string::npos);
    return true;
}
bool epub_name(std::string name) {
    for (auto &c : name)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return name.size() > 5 && name.substr(name.size() - 5) == ".epub";
}
} // namespace

struct ReaderApp::Impl final : DisplayBackend {
    std::string root, writer, book_path, folder, message, last_book;
    FileStore store;
    graphics::Canvas canvas;
    graphics::Font ui, body;
    Display display{*this};
    Session session{display};
    InputRouter router;
    EpubDocument document;
    Chapter chapter;
    graphics::Page page;
    ReaderSettings settings, draft;
    SettingsSnapshot edit_snapshot;
    View view = View::Library;
    std::optional<Frame> pending;
    std::optional<NetworkOperation> network_request;
    std::shared_ptr<std::atomic_bool> network_cancel;
    std::optional<Anchor> remote_anchor;
    std::optional<ReadingRecord> upload_record;
    sync::Config sync_config;
    std::uint64_t network_sequence = 0, active_network = 0;
    bool sync_checked = false, uploading = false;
    std::string sync_status;
    std::vector<Entry> entries;
    std::vector<Hit> hits, links;
    std::optional<Anchor> link_return, confirmed_link_return;
    std::map<std::size_t, std::vector<std::size_t>> page_starts;
    std::size_t chapter_index = 0, offset = 0, library_page = 0, toc_page = 0;
    std::vector<std::size_t> toc_starts;
    std::uint64_t ui_sequence = 0;
    int parameter = FontSize;
    bool book_scope = true, card_available = true, font_fallback = false, builtin_body = false,
         advanced = false, reset_overrides = false;
    unsigned body_updates = 0;
    std::string effective_font, last_recorded;
    std::vector<std::string> fonts;

    Impl(std::string card, std::string id)
        : root(std::move(card)), writer(std::move(id)), store(root) {}
    bool start(const Frame &frame) override {
        pending = frame;
        return true;
    }
    bool has_book() const {
        return !book_path.empty();
    }
    bool persist() {
        auto request = session.request_save();
        if (!request)
            return !session.needs_save();
        const auto result = store.save_progress(book_path, request->record);
        const bool okay =
            result.status == StoreStatus::Ok && result.record && *result.record == request->record;
        session.saved(*request, okay);
        if (!okay)
            message = "进度未保存，请检查 TF 卡后重试";
        return okay;
    }
    void text(const std::string &value, int x, int top, int size = 28, int max_width = 612,
              std::uint8_t gray = 1) {
        if (!ui.size(size))
            return;
        const int baseline = top + ui.metrics().ascender;
        const auto scalars = decode_utf8(value);
        std::vector<int> widths;
        int total = 0;
        for (auto scalar : scalars) {
            const int width = ui.advance(scalar);
            widths.push_back(width);
            total += width;
        }
        const int ellipsis = ui.advance(U'…');
        const bool clipped = total > max_width;
        int used = 0;
        for (std::size_t i = 0; i < scalars.size(); ++i) {
            if (used + widths[i] > (clipped ? max_width - ellipsis : max_width)) {
                ui.draw(canvas, U'…', x + used, baseline, gray);
                break;
            }
            ui.draw(canvas, scalars[i], x + used, baseline, gray);
            used += widths[i];
        }
    }
    int wrapped(const std::string &value, int x, int top, int width, int size = 26,
                bool draw = true) {
        ui.size(size);
        const auto metrics = ui.metrics();
        const int line = metrics.ascender + metrics.descender + 2;
        int used = 0, rows = 1;
        for (const auto scalar : decode_utf8(value)) {
            const int advance = ui.advance(scalar);
            if (used + advance > width) {
                used = 0;
                ++rows;
            }
            if (draw)
                ui.draw(canvas, scalar, x + used, top + (rows - 1) * line + metrics.ascender, 1);
            used += advance;
        }
        return rows * line;
    }
    void center(const std::string &value, int x, int top, int w, int size = 28,
                std::uint8_t gray = 1) {
        ui.size(size);
        int length = 0;
        for (auto c : decode_utf8(value))
            length += ui.advance(c);
        text(value, x + std::max(0, (w - length) / 2), top, size, w, gray);
    }
    void outline(int x, int y, int w, int h, int weight = 2) {
        canvas.fill_rect(x, y, w, weight, 1);
        canvas.fill_rect(x, y + h - weight, w, weight, 1);
        canvas.fill_rect(x, y, weight, h, 1);
        canvas.fill_rect(x + w - weight, y, weight, h, 1);
    }
    void button(int id, const std::string &title, int x, int y, int w, int h = 88,
                bool selected = false, int size = 28) {
        if (selected)
            canvas.fill_rect(x, y, w, h, 1);
        else
            outline(x, y, w, h);
        center(title, x, y + (h - size) / 2 - 3, w, size, selected ? 15 : 1);
        hits.push_back({x, y, w, h, id});
    }
    void header(const std::string &title, int back = Back) {
        canvas.fill_rect(0, 0, 684, 104, 15);
        canvas.line(0, 103, 683, 103, 12);
        if (back) {
            text("‹ 返回", 28, 32, 28, 130);
            hits.push_back({0, 0, 170, 104, back});
        }
        center(title, 174, 28, 336, 34);
    }
    void pager(int previous, int next, std::size_t index, std::size_t total) {
        canvas.fill_rect(0, 1096, 684, 120, 15);
        canvas.line(0, 1096, 683, 1096, 12);
        if (index > 0)
            button(previous, "上一页", 24, 1112, 180);
        center(std::to_string(index + 1) + " / " + std::to_string(std::max<std::size_t>(1, total)),
               216, 1138, 252, 26);
        if (index + 1 < total)
            button(next, "下一页", 480, 1112, 180);
    }
    void note(const std::string &value, int y = 1020) {
        text(value, 36, y, 24, 612, 3);
    }
    void overlay(bool full = false) {
        display.present({{"ui", 0, 0, ++ui_sequence},
                         {},
                         FramePurpose::Overlay,
                         full ? RefreshIntent::FullClean : RefreshIntent::Content});
    }
    graphics::LayoutSpec layout(const ReaderSettings &cfg, int top = 64, int bottom = footer_top) {
        graphics::LayoutSpec spec;
        spec.left = cfg.margin_x_px;
        spec.top = top;
        spec.width = 684 - 2 * spec.left;
        spec.height = bottom - top;
        spec.font_px = cfg.font_size_px;
        body.size(spec.font_px);
        spec.indent_chars = cfg.indent_chars;
        spec.paragraph_gap = static_cast<int>(
            std::lround(cfg.paragraph_gap_lines * cfg.font_size_px * cfg.line_height));
        spec.line_height = std::max(static_cast<int>(std::lround(cfg.line_height * spec.font_px)),
                                    body.metrics().ascender + body.metrics().descender);
        return spec;
    }
    void load_fonts(const ReaderSettings *preview = nullptr) {
        const auto &selected = preview ? *preview : settings;
        if (effective_font == selected.font && !builtin_body)
            return;
        font_fallback = false;
        builtin_body = false;
        const std::string path = root + "/PicoPage/" + selected.font;
        if (!body.open(path)) {
            if (!body.open(root + "/PicoPage/fonts/LXGWWenKai-Regular.ttf")) {
                body.open_fallback();
                builtin_body = true;
            }
            font_fallback = true;
        }
        effective_font = selected.font;
    }
    bool initialize() {
        if (!ui.open(root + "/PicoPage/fonts/NotoSansSC-Regular.otf") && !ui.open_fallback())
            return false;
        settings = store.load_settings().settings;
        load_fonts();
        std::string detail;
        const auto recent = store.load_last_book(detail);
        if (recent) {
            last_book = *recent;
            last_recorded = *recent;
        }
        DIR *dir = opendir((root + "/PicoPage/fonts").c_str());
        if (dir) {
            while (const auto *item = readdir(dir)) {
                const std::string name = item->d_name;
                if (name.size() > 4 && (name.substr(name.size() - 4) == ".ttf" ||
                                        name.substr(name.size() - 4) == ".otf"))
                    fonts.push_back("fonts/" + name);
            }
            closedir(dir);
            std::sort(fonts.begin(), fonts.end());
        }
        if (std::find(fonts.begin(), fonts.end(), settings.font) == fonts.end())
            fonts.push_back(settings.font);
        scan_library();
        draw_library();
        overlay(true);
        return true;
    }
    void scan_library() {
        entries.clear();
        if (!card_available)
            return;
        const auto cfg = store.load_settings();
        if (folder.empty()) {
            for (const auto &path : cfg.settings.library_roots) {
                struct stat st{};
                if (safe_existing(root, path) && stat((root + "/" + path).c_str(), &st) == 0 &&
                    S_ISDIR(st.st_mode))
                    entries.push_back({path, basename(path), true});
            }
        } else {
            DIR *dir = opendir((root + "/" + folder).c_str());
            if (!dir) {
                message = "无法读取目录，请检查 TF 卡";
                return;
            }
            while (const auto *item = readdir(dir)) {
                const std::string name = item->d_name;
                if (name.empty() || name[0] == '.' || name.size() > 240 ||
                    name.find(".picopage") != std::string::npos)
                    continue;
                const auto path = folder + "/" + name;
                struct stat st{};
#ifdef ESP_PLATFORM
                const int result = stat((root + "/" + path).c_str(), &st);
#else
                const int result = lstat((root + "/" + path).c_str(), &st);
#endif
                if (result != 0)
                    continue;
                if (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && epub_name(name)))
                    entries.push_back({path, name, S_ISDIR(st.st_mode)});
                if (entries.size() >= 512) {
                    message = "当前目录仅显示前 512 项，请分目录存放";
                    break;
                }
            }
            closedir(dir);
        }
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            if (a.directory != b.directory)
                return a.directory > b.directory;
            return a.name < b.name;
        });
        library_page = std::min(library_page, entries.empty() ? 0 : (entries.size() - 1) / 6);
    }
    void draw_library() {
        view = View::Library;
        hits.clear();
        canvas.clear();
        text(folder.empty() ? "书库" : basename(folder), 36, 28, 34, 330);
        if (!folder.empty())
            hits.push_back({0, 0, 350, 104, Back});
        button(Import, "导入", 420, 18, 106, 68, false, 26);
        button(Settings, "设置", 542, 18, 106, 68, false, 26);
        canvas.line(0, 103, 683, 103, 12);
        text("最近阅读", 36, 128, 24, 612, 3);
        outline(24, 176, 636, 170, 3);
        if (has_book() || !last_book.empty()) {
            text(has_book() ? document.metadata().title : basename(last_book), 48, 202, 32, 570);
            text("继续阅读", 48, 282, 26);
            hits.push_back({24, 176, 636, 170, Tools});
        } else {
            text("从书库打开一本 EPUB", 48, 208, 30, 570);
            text("阅读位置会自动保存在 TF 卡", 48, 276, 24, 570, 3);
        }
        text(folder.empty() ? "本机书籍" : folder, 36, 378, 28, 440);
        text(std::to_string(entries.size()) + " 项", 542, 380, 24, 108, 3);
        const auto begin = library_page * 6;
        for (std::size_t i = begin; i < std::min(begin + 6, entries.size()); ++i) {
            const int y = 422 + static_cast<int>(i - begin) * 108;
            text(entries[i].directory ? "▱" : "EPUB", 36, y + 30, entries[i].directory ? 34 : 20,
                 74, 3);
            text(entries[i].name, 124, y + 25, 28, 490);
            text(entries[i].directory ? "文件夹" : "离线阅读", 124, y + 64, 22, 480, 3);
            canvas.line(36, y + 107, 648, y + 107, 12);
            hits.push_back({24, y, 636, 108, EntryBase + static_cast<int>(i)});
        }
        if (entries.empty()) {
            note(card_available ? "没有 EPUB。请先将文件复制到 Books 目录。" : "未找到 TF 卡。",
                 490);
        }
        if (!message.empty()) {
            canvas.fill_rect(24, 1040, 636, 44, 15);
            note(message, 1044);
        }
        pager(Prev, Next, library_page, (entries.size() + 5) / 6);
    }
    bool open(std::string path) {
        if (display.busy() || !card_available)
            return false;
        if (!safe_existing(root, path)) {
            error("书籍路径无效或包含符号链接");
            return false;
        }
        if (has_book() && !persist()) {
            error(message);
            return false;
        }
        std::string detail;
        if (!document.open(root + "/" + path, detail)) {
            book_path.clear();
            session.open("", "", writer);
            error("无法打开 EPUB：" + detail);
            return false;
        }
        cancel_sync();
        link_return.reset();
        confirmed_link_return.reset();
        book_path = path;
        last_book = path;
        const auto configuration = store.load_settings(path);
        settings = configuration.settings;
        effective_font.clear();
        load_fonts();
        page_starts.clear();
        const auto restored =
            store.load_progress(path, document.metadata().book_id, content_revision);
        session.open(document.metadata().book_id, content_revision, writer, restored.record);
        if (restored.status != StoreStatus::Ok && restored.status != StoreStatus::Missing) {
            error("原进度读取失败，已停止开书以保护记录");
            return false;
        }
        chapter_index = 0;
        offset = 0;
        message.clear();
        if (!configuration.diagnostics.empty())
            message = "设置文件无效，已回退；原文件未修改";
        else if (font_fallback)
            message = "所选字体不可用，已使用替代字体";
        if (restored.status != StoreStatus::Ok && restored.status != StoreStatus::Missing)
            message = "原进度无法读取，未覆盖原记录";
        if (restored.record) {
            if (!document.chapter_index_for(restored.record->anchor.resource, chapter_index) ||
                !document.load_chapter(chapter_index, chapter, detail) ||
                !chapter.offset_for(restored.record->anchor, restored_offset)) {
                error("保存的位置无法映射，请保留进度文件并检查 EPUB");
                return false;
            }
            offset = restored_offset;
        } else {
            std::size_t first = 0;
            while (first + 1 < document.spine().size() && !document.spine()[first].linear)
                ++first;
            if (!load_chapter(first))
                return false;
        }
        return show_page(offset);
    }
    bool recover_body() {
        const auto record =
            session.visible_record() ? session.visible_record() : session.saved_record();
        if (!record) {
            draw_library();
            overlay();
            return false;
        }
        std::size_t index;
        std::uint32_t position;
        if (!document.chapter_index_for(record->anchor.resource, index) || !load_chapter(index) ||
            !chapter.offset_for(record->anchor, position)) {
            error("已显示位置无法恢复");
            return false;
        }
        return show_page(position);
    }
    std::uint32_t restored_offset = 0;
    bool load_chapter(std::size_t index) {
        std::string detail;
        Chapter candidate;
        if (!document.load_chapter(index, candidate, detail)) {
            error("章节读取失败：" + detail);
            return false;
        }
        chapter = std::move(candidate);
        chapter_index = index;
        offset = 0;
        return true;
    }
    void footer() {
        canvas.fill_rect(0, footer_top, 684, 88, 15);
        std::string chapter_title = "章节 " + std::to_string(chapter_index + 1);
        for (const auto &item : document.toc())
            if (item.resource == chapter.resource) {
                chapter_title = item.label;
                break;
            }
        text(message.empty() ? chapter_title : message, 36, 1150, 22, 430, 3);
        text(std::to_string(chapter_index + 1) + " / " + std::to_string(document.spine().size()),
             500, 1150, 22, 148, 3);
    }
    std::size_t readable_offset(std::size_t start) const {
        // Inserted block separators have no source anchor for a saved page start.
        Anchor anchor;
        while (start < chapter.text.size() &&
               !chapter.anchor_at(static_cast<std::uint32_t>(start), anchor))
            ++start;
        return start;
    }
    const DocumentImage* image_at(std::size_t start) const {
        for (const auto& image : chapter.images)
            if (image.begin <= start && start < image.end && image.end <= chapter.text.size())
                return &image;
        return nullptr;
    }
    graphics::Page plan_page(std::size_t start, const graphics::LayoutSpec& spec) {
        // Media ranges occupy one page; text layout must not consume their placeholders.
        start = readable_offset(start);
        if (const auto* image = image_at(start)) {
            graphics::Page result;
            result.start = image->begin;
            result.end = image->end;
            result.font_px = spec.font_px;
            return result;
        }
        auto end = chapter.text.size();
        for (const auto& image : chapter.images)
            if (image.begin > start && image.begin < image.end && image.end <= chapter.text.size())
                end = std::min<std::size_t>(end, image.begin);
        return graphics::layout_page(body, std::u32string_view(chapter.text).substr(0, end), start, spec);
    }
    bool draw_page_content(const graphics::Page& planned, const graphics::LayoutSpec& spec) {
        if (const auto* image = image_at(planned.start)) {
            std::vector<std::uint8_t> encoded;
            std::string detail;
            if (document.read_image(image->resource, encoded, detail) &&
                graphics::render_image(canvas, encoded, spec.left, spec.top, spec.width, spec.height, detail))
                return true;
            outline(spec.left, spec.top + 12, spec.width, spec.height - 24, 2);
            center("图片无法显示", spec.left + 8, spec.top + spec.height / 2 - 36, spec.width - 16, 28);
            center("图片缺失、损坏或格式不支持", spec.left + 8, spec.top + spec.height / 2 + 16,
                   spec.width - 16, 22, 3);
            return true;
        }
        return graphics::render_page(canvas, body, planned);
    }
    bool render_body(std::size_t start) {
        const auto spec = layout(settings, link_return ? 144 : 64);
        auto candidate = plan_page(start, spec);
        if (!candidate.error.empty() || candidate.end <= candidate.start) {
            error("本页无法排版：" + candidate.error);
            return false;
        }
        for (const auto &glyph : candidate.glyphs)
            if (!body.contains(glyph.scalar)) {
                if (builtin_body) {
                    error("正文阅读字体缺失，请复制字体到 PicoPage/fonts 后重新打开书籍。");
                    return false;
                }
                message = "部分字符缺字，请更换正文阅读字体";
            }
        canvas.clear();
        if (!draw_page_content(candidate, spec)) {
            error("正文绘制失败：" + body.error());
            return false;
        }
        page = std::move(candidate);
        offset = page.start;
        footer();
        links.clear();
        for (std::size_t i = 0; i < chapter.links.size(); ++i) {
            const auto &link = chapter.links[i];
            if (link.end <= page.start || link.begin >= page.end)
                continue;
            for (const auto &line : page.lines) {
                int left = 684, right = 0;
                for (const auto &glyph : page.glyphs)
                    if (glyph.baseline == line.baseline && glyph.index >= link.begin &&
                        glyph.index < link.end) {
                        left = std::min(left, glyph.x);
                        right = std::max(right, glyph.x + body.advance(glyph.scalar));
                    }
                if (right > left) {
                    canvas.line(left, line.bottom, right, line.bottom, 3);
                    links.push_back({left, line.top, right - left, line.bottom - line.top + 2,
                                     LinkBase + static_cast<int>(i)});
                }
            }
        }
        if (link_return) {
            canvas.fill_rect(0, 0, 684, 104, 15);
            text("‹ 返回原位置", 28, 30, 28, 612);
            canvas.line(0, 102, 683, 102, 1, 3);
            links.push_back({0, 0, 684, 104, LinkReturn});
        }
        return true;
    }
    bool show_page(std::size_t start) {
        if (display.busy())
            return false;
        load_fonts();
        if (!render_body(start))
            return false;
        Anchor anchor;
        std::size_t p = page.start;
        while (p < page.end && !chapter.anchor_at(static_cast<std::uint32_t>(p), anchor))
            ++p;
        if (p == page.end) {
            error("本页没有可保存的来源位置");
            return false;
        }
        auto &starts = page_starts[chapter_index];
        if (std::find(starts.begin(), starts.end(), page.start) == starts.end()) {
            starts.push_back(page.start);
            std::sort(starts.begin(), starts.end());
        }
        view = View::Reading;
        hits.clear();
        const auto token = session.request_page(
            anchor, (settings.refresh_preset == "clear" ||
                     body_updates >= (settings.refresh_preset == "fast" ? fast_full_refresh_pages
                                                                         : balanced_full_refresh_pages))
                        ? RefreshIntent::FullClean
                        : RefreshIntent::Content);
        if (!session.layout_ready(token))
            return false;
        return true;
    }
    void turn(bool forward) {
        if (!has_book())
            return;
        if (forward) {
            const auto next_offset = readable_offset(page.end);
            if (next_offset < chapter.text.size())
                show_page(next_offset);
            else if (chapter_index + 1 < document.spine().size()) {
                auto next = chapter_index + 1;
                while (next < document.spine().size() && !document.spine()[next].linear)
                    ++next;
                if (next < document.spine().size() && load_chapter(next))
                    show_page(0);
            } else {
                message = "已到全书末尾";
                draw_tools();
                overlay();
            }
        } else {
            if (offset == readable_offset(0)) {
                if (chapter_index == 0)
                    return;
                auto previous_chapter = chapter_index - 1;
                while (previous_chapter > 0 && !document.spine()[previous_chapter].linear)
                    --previous_chapter;
                if (!document.spine()[previous_chapter].linear || !load_chapter(previous_chapter))
                    return;
                std::size_t start = 0, previous = 0;
                while (start < chapter.text.size()) {
                    auto p = plan_page(start, layout(settings, link_return ? 144 : 64));
                    if (p.start == chapter.text.size())
                        break;
                    if (p.end <= start || !p.error.empty()) {
                        error("上一章无法排版");
                        return;
                    }
                    previous = p.start;
                    start = p.end;
                }
                show_page(previous);
            } else {
                const auto &starts = page_starts[chapter_index];
                auto i = std::lower_bound(starts.begin(), starts.end(), offset);
                if (i != starts.begin()) {
                    show_page(*std::prev(i));
                    return;
                }
                std::size_t start = 0, previous = 0;
                while (start < offset) {
                    auto p = plan_page(start, layout(settings, link_return ? 144 : 64));
                    if (p.start >= offset)
                        break;
                    if (p.end <= start || !p.error.empty()) {
                        error("上一页无法排版");
                        return;
                    }
                    previous = p.start;
                    start = p.end;
                }
                show_page(previous);
            }
        }
    }
    void draw_tools() {
        view = View::Tools;
        hits.clear();
        if (!render_body(offset))
            return;
        for (const auto &line : page.lines) {
            if ((line.top < 104 && line.bottom > 104) ||
                (line.top < toolbar_top && line.bottom > toolbar_top))
                canvas.fill_rect(0, line.top, 684, line.bottom - line.top + 1, 15);
        }
        canvas.fill_rect(0, 0, 684, 104, 15);
        text("‹ 书库", 28, 30, 28, 150);
        hits.push_back({0, 0, 170, 104, Library});
        text(document.metadata().title, 190, 18, 28, 460);
        text("章节 " + std::to_string(chapter_index + 1), 190, 58, 22, 450, 3);
        canvas.line(0, 103, 683, 103, 12);
        canvas.fill_rect(0, toolbar_top, 684, 136, 15);
        canvas.line(0, toolbar_top, 683, toolbar_top, 1);
        const char *labels[] = {"目录", "排版", "同步", "刷新", "全刷"};
        const int ids[] = {Contents, Typography, Sync, Refresh, FullRefresh};
        for (int i = 0; i < 5; ++i) {
            if (i)
                canvas.line(i * 137, toolbar_top, i * 137, 1215, 12);
            center(labels[i], i * 137, 1142, 137, 28);
            hits.push_back({i * 137, toolbar_top, 137, 136, ids[i]});
        }
        if (!message.empty()) {
            canvas.fill_rect(24, 1012, 636, 56, 15);
            note(message, 1024);
        }
        hits.push_back({0, 104, 684, 908, Dismiss});
    }
    void draw_contents() {
        view = View::Contents;
        hits.clear();
        canvas.clear();
        header("目录");
        text(document.metadata().title, 36, 134, 28, 612);
        canvas.line(0, 199, 683, 199, 12);
        const auto &toc = document.toc();
        toc_starts = {0};
        int used = 0;
        auto row_height = [&](const TocEntry &item) {
            const int indent = std::min<int>(item.depth, 3) * 24;
            return std::max(88, wrapped(item.label, 0, 0, 570 - indent, 26, false) + 24);
        };
        for (std::size_t i = 0; i < toc.size(); ++i) {
            const int height = std::min(880, row_height(toc[i]));
            if (used && used + height > 880) {
                toc_starts.push_back(i);
                used = 0;
            }
            used += height;
        }
        toc_page = std::min(toc_page, toc_starts.size() - 1);
        const auto begin = toc_starts[toc_page];
        const auto end = toc_page + 1 < toc_starts.size() ? toc_starts[toc_page + 1] : toc.size();
        int y = 200;
        for (std::size_t i = begin; i < end; ++i) {
            const int h = std::min(880, row_height(toc[i]));
            const int indent = std::min<int>(toc[i].depth, 3) * 24;
            if (toc[i].resource == chapter.resource)
                outline(24, y + 3, 636, h - 6, 3);
            wrapped(toc[i].label, 48 + indent, y + 12, 570 - indent, 26);
            canvas.line(36, y + h - 1, 648, y + h - 1, 12);
            hits.push_back({24, y, 636, h, TocBase + static_cast<int>(i)});
            y += h;
        }
        if (toc.empty())
            note("此书没有目录，可使用 K1 / K3 翻页。", 256);
        pager(Prev, Next, toc_page, toc_starts.size());
    }
    void draw_typography() {
        view = View::Typography;
        hits.clear();
        canvas.clear();
        load_fonts(&draft);
        const auto spec = layout(draft, 64, 656);
        const auto preview = plan_page(offset, spec);
        if (!preview.error.empty() || !draw_page_content(preview, spec))
            text("预览暂不可用", 36, 96, 28);
        canvas.fill_rect(16, 12, 270, 40, 15);
        outline(16, 12, 270, 40);
        text("预览 · 尚未应用", 28, 16, 20, 246);
        canvas.fill_rect(0, 656, 684, 560, 15);
        canvas.line(0, 656, 683, 656, 1, 3);
        const int tab_ids[] = {Contents, Typography, Sync, Refresh, Back};
        const char *tab_names[] = {"目录", "排版", "同步", "刷新", "收起"};
        for (int i = 0; i < 5; ++i) {
            const int x = i * 143, w = i == 4 ? 112 : 143;
            if (i == 1)
                canvas.fill_rect(x, 660, w, 96, 1);
            canvas.line(x, 660, x, 756, 1);
            center(tab_names[i], x, 686, w, 26, i == 1 ? 15 : 1);
            hits.push_back({x, 660, w, 96, tab_ids[i]});
        }
        std::vector<int> ids = advanced
                                   ? std::vector<int>{Gap, Indent}
                                   : std::vector<int>{FontChoice, FontSize, LineHeight, Margin};
        auto name = [](int id) -> std::string {
            switch (id) {
            case FontChoice:
                return "字体";
            case FontSize:
                return "字号";
            case LineHeight:
                return "行距";
            case Margin:
                return "页边距";
            case Gap:
                return "段距";
            default:
                return "首行缩进";
            }
        };
        auto value = [&](int id) -> std::string {
            switch (id) {
            case FontChoice:
                return draft.font.find("WenKai") != std::string::npos     ? "文楷"
                       : draft.font.find("NotoSans") != std::string::npos ? "思源黑体"
                                                                          : basename(draft.font);
            case FontSize:
                return std::to_string(draft.font_size_px) + " px";
            case LineHeight:
                return decimal(draft.line_height);
            case Margin:
                return std::to_string(draft.margin_x_px) + " px";
            case Gap:
                return decimal(draft.paragraph_gap_lines) + " 行";
            default:
                return std::to_string(draft.indent_chars) + " 字";
            }
        };
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const int x = 24 + static_cast<int>(i) * 162, w = 150;
            if (parameter == ids[i])
                outline(x, 776, w, 96, 3);
            center(name(ids[i]), x, 788, w, 22);
            center(value(ids[i]), x, 830, w, 24);
            hits.push_back({x, 776, w, 96, ids[i]});
        }
        if (advanced)
            button(Advanced, "‹ 基本", 486, 776, 174, 96, false, 24);
        button(Minus, "−", 36, 904, 120, 88, false, 40);
        center(name(parameter), 168, 900, 348, 22);
        center(value(parameter), 168, 946, 348, 34);
        button(Plus, "+", 528, 904, 120, 88, false, 40);
        canvas.line(0, 1012, 683, 1012, 12);
        text("作用域：此书", 36, 1034, 22, 190);
        text("段距、缩进 >", 236, 1034, 22, 238);
        hits.push_back({220, 1014, 258, 72, Advanced});
        text("恢复默认", 514, 1034, 22, 146);
        hits.push_back({494, 1014, 166, 72, Defaults});
        button(Scope, "设为阅读默认", 36, 1088, 298, 96, false, 26);
        if (!font_fallback)
            button(Apply, "应用", 350, 1088, 298, 96, true);
        else {
            outline(350, 1088, 298, 96);
            center("字体不可用", 350, 1116, 298, 26, 5);
            canvas.fill_rect(24, 586, 636, 58, 15);
            note("字体缺失或损坏，预览使用替代字体。", 596);
        }
    }
    static std::string decimal(double value) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.2f", value);
        return buf;
    }
    void draw_refresh() {
        view = View::Refresh;
        hits.clear();
        canvas.clear();
        header("刷新设置");
        text("选择阅读刷新方式", 36, 148, 32);
        const auto& preset = edit_snapshot.config.settings.refresh_preset;
        button(RefreshClear, "清晰", 24, 224, 636, 112, preset == "clear");
        note("每页执行全刷，减少残影。", 354);
        button(RefreshBalanced, "均衡", 24, 436, 636, 112, preset == "balanced");
        note("翻页使用灰阶更新，定期执行全刷。", 566);
        button(RefreshFast, "快速", 24, 648, 636, 112, preset == "fast");
        note("翻页使用更快的灰阶更新，每 " + std::to_string(fast_full_refresh_pages) +
                 " 页执行全刷。",
             778);
        button(FullRefresh, "立即全刷", 24, 960, 636, 96);
        note("正文中按 K2，可随时立即全刷。", 1088);
    }
    void draw_settings() {
        view = View::Settings;
        hits.clear();
        canvas.clear();
        header("设置");
        button(Sync, "阅读同步", 24, 160, 636, 96);
        button(Import, "文件导入", 24, 288, 636, 96);
        text("Pico Page", 36, 448, 34);
        note("EPUB 离线阅读初版", 510);
        note("阅读进度与排版设置保存在 TF 卡。", 574);
        note("设置文件：PicoPage/settings.toml", 638);
        note("关机前请确认阅读进度保存成功。", 702);
        if (!message.empty())
            note(message, 884);
        if (session.needs_save())
            button(SaveRetry, "重试保存进度", 24, 960, 636, 96);
    }
    void draw_import() {
        view = View::Import;
        hits.clear();
        canvas.clear();
        header("导入书籍");
        text("通过 TF 卡导入", 36, 160, 34);
        note("1. 返回书库，确认进度已保存。", 246);
        note("2. 关闭设备，取出 TF 卡。", 320);
        note("3. 将 EPUB 复制到 Books 目录。", 394);
        note("4. 插回 TF 卡，再开机。", 468);
        note("字体放在 PicoPage/fonts 目录。", 602);
        note("不支持 DRM 加密书籍。", 676);
        button(Dismiss, "返回书库", 24, 1056, 636, 96);
    }
    static bool same_account(const sync::Config &a, const sync::Config &b) {
        return a.endpoint == b.endpoint && a.username == b.username && a.userkey == b.userkey &&
               a.device_id == b.device_id && a.dom_version == b.dom_version;
    }
    void cancel_sync() {
        if (network_cancel)
            network_cancel->store(true);
        active_network = 0;
        network_request.reset();
        remote_anchor.reset();
        upload_record.reset();
        sync_checked = false;
    }
    void draw_sync() {
        view = View::Sync;
        hits.clear();
        canvas.clear();
        header("阅读同步");
        text("KOReader 接续", 36, 160, 34);
        if (!has_book()) {
            note("请先打开一本 EPUB。", 246);
            return;
        }
        if (sync_status.empty())
            sync_status = "点击检查，获取此书的远端位置。";
        wrapped(sync_status, 36, 250, 612, 26);
        note("本地位置只在成功显示并保存后上传。", 456);
        if (active_network) {
            note(uploading ? "正在上传已保存的位置…" : "正在检查远端位置…", 600);
            if (uploading)
                note("返回后，已发出的上传仍可能生效。", 684);
            button(Back, "取消并返回", 24, 1040, 636, 96);
            return;
        }
        button(SyncCheck, "检查远端位置", 24, 572, 636, 96);
        if (remote_anchor)
            button(SyncJump, "跳转到远端位置", 24, 704, 636, 96, true);
        if (sync_checked)
            button(SyncUpload, "上传本机位置", 24, 836, 636, 96);
        note("服务配置：PicoPage/sync.toml", 994);
        note("联网配置：PicoPage/network.toml", 1056);
    }
    void check_sync() {
        cancel_sync();
        std::string detail;
        if (!sync::load_config(root, sync_config, detail)) {
            sync_status = "同步未配置或配置无效：" + detail;
            draw_sync();
            overlay();
            return;
        }
        sync::HttpRequest request;
        if (!sync::get_request(sync_config, document.metadata().partial_md5, request, detail)) {
            sync_status = detail;
            draw_sync();
            overlay();
            return;
        }
        active_network = ++network_sequence;
        uploading = false;
        network_cancel = std::make_shared<std::atomic_bool>(false);
        network_request = NetworkOperation{active_network, std::move(request), network_cancel};
        sync_status = "正在检查远端位置";
        draw_sync();
        overlay();
    }
    void confirm_upload() {
        view = View::Confirmation;
        hits.clear();
        canvas.clear();
        header("确认上传", 0);
        text("用本机位置更新远端？", 36, 218, 34);
        note("仅上传当前已保存的阅读位置。", 326);
        note("其他设备下次同步会收到此位置。", 396);
        button(Back, "取消", 24, 1056, 306, 96, true);
        button(SyncConfirm, "确认上传", 354, 1056, 306, 96);
    }
    void upload() {
        if (!sync_checked || !persist() || !session.saved_record()) {
            sync_status = "当前进度尚未保存，无法上传";
            draw_sync();
            overlay();
            return;
        }
        std::string detail;
        sync::Config current;
        if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
            cancel_sync();
            sync_status = "同步配置已更改，请重新检查远端位置";
            draw_sync();
            overlay();
            return;
        }
        const auto record = *session.saved_record();
        std::string xpointer;
        Chapter source;
        std::size_t index;
        if (!document.chapter_index_for(record.anchor.resource, index) ||
            !document.load_chapter(index, source, detail) ||
            !sync::to_xpointer(document, source, record.anchor, sync_config.dom_version, xpointer,
                               detail)) {
            sync_status = "此位置暂不支持精确同步：" + detail;
            draw_sync();
            overlay();
            return;
        }
        std::uint32_t scalar = 0;
        source.offset_for(record.anchor, scalar);
        const double percent =
            (static_cast<double>(index) +
             (source.text.empty() ? 0. : static_cast<double>(scalar) / source.text.size())) /
            document.spine().size();
        sync::HttpRequest request;
        if (!sync::put_request(sync_config, document.metadata().partial_md5, xpointer, percent,
                               record, request, detail)) {
            sync_status = detail;
            draw_sync();
            overlay();
            return;
        }
        upload_record = record;
        uploading = true;
        active_network = ++network_sequence;
        network_cancel = std::make_shared<std::atomic_bool>(false);
        network_request = NetworkOperation{active_network, std::move(request), network_cancel};
        sync_status = "正在上传已保存的位置";
        draw_sync();
        overlay();
    }
    void network_complete(std::uint64_t id, int status, const std::string &bytes,
                          const std::string &failure) {
        if (id != active_network || !active_network)
            return;
        active_network = 0;
        if (view != View::Sync) {
            cancel_sync();
            return;
        }
        sync::Config current;
        std::string detail;
        if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
            cancel_sync();
            sync_status = "同步配置已更改，本次结果已丢弃";
        } else if (!failure.empty()) {
            sync_checked = false;
            remote_anchor.reset();
            sync_status = "同步失败：" + failure;
        } else if (uploading) {
            if (sync::upload_confirmed(status, detail) && upload_record && session.saved_record() &&
                *session.saved_record() == *upload_record)
                sync_status = "已上传此版本的阅读位置";
            else
                sync_status = "尚未确认当前版本已上传：" + detail;
            upload_record.reset();
            sync_checked = false;
        } else {
            const auto remote = sync::parse_remote(status, bytes);
            if (remote.status == sync::RemoteStatus::NoRecord) {
                sync_checked = true;
                sync_status = "远端没有此书的阅读记录";
            } else if (remote.status == sync::RemoteStatus::Found) {
                Anchor candidate;
                sync_checked = true;
                if (sync::from_xpointer(document, remote.progress, sync_config.dom_version,
                                        candidate, detail)) {
                    remote_anchor = candidate;
                    if (session.saved_record() && session.saved_record()->anchor == candidate) {
                        remote_anchor.reset();
                        sync_status = "本机与远端位于同一源位置";
                    } else
                        sync_status = "发现远端位置。选择跳转，或保留本机位置。";
                } else {
                    sync_status = "远端位置无法精确映射，未跳转：" + detail;
                }
            } else {
                sync_checked = false;
                sync_status = "无法读取远端位置：" + remote.error;
            }
        }
        draw_sync();
        overlay();
    }
    void error(const std::string &detail) {
        cancel_sync();
        link_return = confirmed_link_return;
        message = detail;
        view = View::Error;
        hits.clear();
        canvas.clear();
        header("暂时无法继续", 0);
        text("请检查后重试", 36, 190, 34);
        auto chars = decode_utf8(detail);
        std::string line;
        int y = 300;
        ui.size(26);
        int used = 0;
        for (char32_t c : chars) {
            const int advance = ui.advance(c);
            if (used + advance > 600) {
                used = 0;
                y += 48;
            }
            ui.draw(canvas, c, 36 + used, y + ui.metrics().ascender, 1);
            used += advance;
            if (y > 800)
                break;
        }
        button(Back, "返回", 24, 1056, 306, 96);
        if (session.needs_save())
            button(SaveRetry, "重试保存", 354, 1056, 306, 96);
        if (!display.busy())
            overlay();
    }
    void back() {
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
                folder = p == std::string::npos ? "" : folder.substr(0, p);
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
        if (view == View::Error && session.needs_save()) {
            draw_settings();
            overlay();
            return;
        }
        if (view == View::Error && has_book()) {
            recover_body();
            return;
        }
        draw_library();
        overlay();
    }
    bool begin_settings_edit() {
        auto snapshot = store.settings_snapshot(book_path);
        if (!snapshot.config.diagnostics.empty()) {
            error("设置文件无效，原文件未修改。请检查后重新打开设置。");
            return false;
        }
        edit_snapshot = std::move(snapshot);
        return true;
    }
    void control(int id) {
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
            if (!load_chapter(index))
                return;
            std::uint32_t target = 0;
            if (!link.fragment.empty() && !chapter.fragment_offset(link.fragment, target)) {
                error("链接锚点不存在");
                return;
            }
            if (!link_return)
                link_return = origin->anchor;
            page_starts.clear();
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
            if (!load_chapter(target))
                return;
            std::uint32_t position = 0;
            if (!item.fragment.empty() && !chapter.fragment_offset(item.fragment, position)) {
                error("目录锚点不存在");
                return;
            }
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
            } else
                open(entry.path);
            return;
        }
        switch (id) {
        case LinkReturn: {
            if (!link_return)
                return;
            const auto target = *link_return;
            std::size_t index;
            std::uint32_t target_offset;
            if (!document.chapter_index_for(target.resource, index) || !load_chapter(index) ||
                !chapter.offset_for(target, target_offset)) {
                error("原位置无法恢复");
                return;
            }
            link_return.reset();
            page_starts.clear();
            show_page(target_offset);
            return;
        }
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
        case Library:
            if (!persist()) {
                error(message);
                return;
            }
            scan_library();
            draw_library();
            overlay();
            return;
        case Tools:
            if (has_book()) {
                if (session.visible_record() || session.saved_record())
                    recover_body();
                else
                    open(book_path);
            } else if (!last_book.empty())
                open(last_book);
            return;
        case Contents:
            toc_page = 0;
            draw_contents();
            break;
        case Typography:
            if (!begin_settings_edit())
                return;
            draft = edit_snapshot.config.settings;
            parameter = FontSize;
            advanced = false;
            reset_overrides = false;
            book_scope = true;
            draw_typography();
            break;
        case Refresh:
            if (!begin_settings_edit())
                return;
            draw_refresh();
            break;
        case Settings:
            draw_settings();
            break;
        case Import:
            draw_import();
            break;
        case Sync:
            cancel_sync();
            sync_status.clear();
            draw_sync();
            break;
        case SyncCheck:
            check_sync();
            return;
        case SyncUpload:
            confirm_upload();
            overlay();
            return;
        case SyncConfirm:
            upload();
            return;
        case SyncJump: {
            if (!remote_anchor)
                return;
            sync::Config current;
            std::string detail;
            if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
                cancel_sync();
                sync_status = "同步配置已更改，请重新检查远端位置";
                draw_sync();
                overlay();
                return;
            }
            const auto target = *remote_anchor;
            std::size_t index;
            std::uint32_t start;
            if (!document.chapter_index_for(target.resource, index) || !load_chapter(index) ||
                !chapter.offset_for(target, start)) {
                error("远端位置无法恢复");
                return;
            }
            cancel_sync();
            link_return.reset();
            page_starts.clear();
            show_page(start);
            return;
        }
        case FullRefresh:
            if (view != View::Reading) {
                load_fonts();
                if (!render_body(offset))
                    return;
                view = View::Reading;
                hits.clear();
            }
            session.force_refresh();
            return;
        case Prev:
        case Next:
            if (view == View::Library) {
                if (id == Prev && library_page)
                    --library_page;
                else if (id == Next && (library_page + 1) * 6 < entries.size())
                    ++library_page;
                draw_library();
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
        case Advanced:
            advanced = !advanced;
            parameter = advanced ? Gap : FontSize;
            draw_typography();
            break;
        case Defaults:
            draft = edit_snapshot.global_settings;
            reset_overrides = true;
            draw_typography();
            break;
        case Minus:
        case Plus: {
            reset_overrides = false;
            const int direction = id == Plus ? 1 : -1;
            if (parameter == FontChoice) {
                auto i = std::find(fonts.begin(), fonts.end(), draft.font);
                auto n = i == fonts.end() ? 0 : static_cast<int>(i - fonts.begin());
                n = (n + direction + static_cast<int>(fonts.size())) %
                    static_cast<int>(fonts.size());
                draft.font = fonts[n];
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
            draw_typography();
            break;
        }
        case Apply:
            apply_typography();
            return;
        case RefreshClear:
        case RefreshBalanced:
        case RefreshFast:
            apply_refresh(id);
            return;
        case SaveRetry:
            if (persist())
                message = "进度已保存";
            draw_settings();
            break;
        default:
            return;
        }
        overlay();
    }
    void apply_typography();
    void apply_refresh(int id);
    Context context() const {
        switch (view) {
        case View::Reading:
            return Context::Reading;
        case View::Tools:
            return Context::Tools;
        case View::Contents:
            return Context::Contents;
        case View::Typography:
            return Context::Typography;
        case View::Confirmation:
            return Context::Confirmation;
        case View::Sleeping:
            return Context::Sleeping;
        case View::Library:
            return folder.empty() ? Context::LibraryRoot : Context::Library;
        default:
            return Context::Tools;
        }
    }
    void input(const AppInput &input) {
        const bool accepted = router.accept(input.event);
        if (!accepted || display.busy() || view == View::Sleeping)
            return;
        if (input.touch) {
            if (view == View::Reading) {
                for (const auto &link : links)
                    if (link.contains(input.x, input.y)) {
                        control(link.id);
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
                        const int id = hit.id;
                        control(id);
                        break;
                    }
            }
            return;
        }
        const auto action = MapInput(context(), input.event.input);
        if (action == Action::ForceRefresh)
            session.force_refresh();
        else if (action == Action::PreviousPage)
            control(Prev);
        else if (action == Action::NextPage)
            control(Next);
        else if (action == Action::Close || action == Action::CancelPreview ||
                 action == Action::Cancel)
            back();
    }
};

void ReaderApp::Impl::apply_typography() {
    std::string detail;
    if (font_fallback) {
        error("所选字体不可用，排版未保存");
        return;
    }
    const auto result = reset_overrides && book_scope
                            ? store.clear_book_settings(edit_snapshot, detail)
                            : store.apply_settings(edit_snapshot, draft, book_scope, detail);
    if (result != StoreStatus::Ok) {
        error("排版未保存：" + detail);
        return;
    }
    const auto effective = store.load_settings(book_path).settings;
    const bool layout_changed = effective.font != settings.font ||
        effective.font_size_px != settings.font_size_px ||
        effective.line_height != settings.line_height ||
        effective.margin_x_px != settings.margin_x_px ||
        effective.paragraph_gap_lines != settings.paragraph_gap_lines ||
        effective.indent_chars != settings.indent_chars;
    settings = effective;
    if (!book_scope) {
        message = layout_changed ? "阅读默认已保存，当前书已应用" : "阅读默认已保存，本书排版未变";
        if (!layout_changed) {
            load_fonts();
            draw_tools();
            overlay();
            return;
        }
    }
    load_fonts();
    page_starts.clear();
    session.change_layout();
    show_page(offset);
}
void ReaderApp::Impl::apply_refresh(int id) {
    auto values = edit_snapshot.config.settings;
    values.refresh_preset = id == RefreshClear ? "clear" : id == RefreshFast ? "fast" : "balanced";
    std::string detail;
    if (store.apply_settings(edit_snapshot, values, true, detail) != StoreStatus::Ok) {
        error("刷新设置未保存：" + detail);
        return;
    }
    // A refresh-only change must not apply an external typography edit to the visible page.
    settings.refresh_preset = values.refresh_preset;
    edit_snapshot = store.settings_snapshot(book_path);
    draw_refresh();
    overlay();
}
ReaderApp::ReaderApp(std::string root, std::string writer)
    : impl_(std::make_unique<Impl>(std::move(root), std::move(writer))) {}
ReaderApp::~ReaderApp() = default;
bool ReaderApp::initialize() {
    return impl_->initialize();
}
bool ReaderApp::open_book(const std::string &path) {
    return impl_->open(path);
}
void ReaderApp::input(const AppInput &event) {
    impl_->input(event);
}
const graphics::Canvas &ReaderApp::canvas() const {
    return impl_->canvas;
}
const std::optional<Frame> &ReaderApp::pending_frame() const {
    return impl_->pending;
}
void ReaderApp::complete_frame(bool success) {
    if (!impl_->pending)
        return;
    const auto frame = *impl_->pending;
    const auto result = impl_->display.finish(frame.token, success);
    impl_->pending.reset();
    if (success) {
        if (frame.refresh == RefreshIntent::FullClean)
            impl_->body_updates = 0;
        else if (frame.purpose == FramePurpose::Body)
            ++impl_->body_updates;
    }
    if (result && impl_->session.presented(*result)) {
        impl_->confirmed_link_return = impl_->link_return;
        if (!impl_->persist())
            impl_->error(impl_->message);
        else if (impl_->last_recorded != impl_->book_path) {
            std::string error;
            if (impl_->store.save_last_book(impl_->book_path, error) != StoreStatus::Ok)
                impl_->message = "最近阅读记录未保存";
            else
                impl_->last_recorded = impl_->book_path;
        }
    }
    if (!success)
        impl_->error("屏幕更新失败，当前页未保存。返回将恢复已确认位置。");
}
View ReaderApp::view() const {
    return impl_->view;
}
const std::string &ReaderApp::status() const {
    return impl_->message;
}
const std::optional<ReadingRecord> &ReaderApp::saved_record() const {
    return impl_->session.saved_record();
}
bool ReaderApp::fast_page_updates() const {
    return impl_->settings.refresh_preset == "fast";
}
int ReaderApp::auto_sleep_minutes() const {
    return impl_->settings.auto_sleep_minutes;
}
bool ReaderApp::prepare_sleep() {
    if (impl_->display.busy() || impl_->active_network || !impl_->persist())
        return false;
    impl_->cancel_sync();
    impl_->view = View::Sleeping;
    impl_->hits.clear();
    impl_->canvas.clear();
    impl_->center("Pico Page", 0, 410, 684, 48);
    impl_->center("已休眠", 0, 508, 684, 32);
    impl_->center("按电源键继续阅读", 0, 584, 684, 26, 3);
    impl_->overlay(true);
    return true;
}
void ReaderApp::wake() {
    if (impl_->display.busy())
        return;
    if (impl_->has_book())
        impl_->show_page(impl_->offset);
    else {
        impl_->draw_library();
        impl_->overlay(true);
    }
}
void ReaderApp::card_removed() {
    impl_->cancel_sync();
    impl_->card_available = false;
    impl_->ui.open_fallback();
    impl_->body.open_fallback();
    impl_->effective_font.clear();
    impl_->error("TF 卡已移除。请重新插卡并重启设备。");
}
void ReaderApp::retry_save() {
    impl_->persist();
}
void ReaderApp::show_error(const std::string &message) {
    if (!impl_->display.busy())
        impl_->error(message);
}
std::optional<NetworkOperation> ReaderApp::take_network_request() {
    if (impl_->display.busy())
        return std::nullopt;
    if (impl_->view != View::Sync || !impl_->active_network || !impl_->network_request ||
        impl_->network_request->id != impl_->active_network)
        return std::nullopt;
    auto request = std::move(impl_->network_request);
    impl_->network_request.reset();
    return request;
}
void ReaderApp::complete_network(std::uint64_t id, int status, const std::string &body,
                                 const std::string &error) {
    impl_->network_complete(id, status, body, error);
}
} // namespace pico_page
