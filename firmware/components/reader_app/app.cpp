#include "content_cache.h"
#include "content_job.h"
#include "pico_page/stage_timer.h"
#include "pico_page/app.h"
#include "pico_page/document.h"
#include "pico_page/provision.h"
#include "pico_page/store.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <map>
#include <set>
#include <sys/stat.h>
#include <utility>
#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#endif

namespace pico_page {
namespace {
using graphics::decode_utf8;
constexpr int footer_top = 1128, toolbar_top = 1080;
constexpr const char *content_revision = "epub-native-v1";
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
    UsbStart,
    UsbExit,
    UsbExitConfirm,
    UsbCancel,
    LibrarySort,
    LibraryList,
    LibraryGrid,
    RefreshAccept,
    RefreshInterval,
    IntervalLess,
    IntervalMore,
    IntervalSave,
    PowerSettings,
    StorageSettings,
    InputSettings,
    SleepCover,
    SleepReading,
    SleepSimple,
    SleepLess,
    SleepMore,
    PowerSave,
    ClearCache,
    DetailOpen,
    DetailCache,
    DetailReset,
    DetailDelete,
    ManageConfirm,
    SyncSettings,
    LibraryView,
    PortalStart,
    PortalStop,
    EntryBase = 100,
    TocBase = 1000,
    TocToggleBase = 10000,
    LinkBase = 20000,
    DetailBase = 30000
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
    bool controlled_storage = false;
    struct VerifiedFile { off_t size; time_t modified; ino_t inode; DocumentMetadata identity; };
    std::map<std::string, VerifiedFile> identities;
    bool open_document(const std::string& path, std::string& error) {
        struct stat before{}, after{};
        const auto full = root + "/" + path;
        const bool stat_ok = stat(full.c_str(), &before) == 0;
        const auto found = identities.find(path);
        const bool reuse = controlled_storage && stat_ok && found != identities.end() &&
            found->second.size == before.st_size && found->second.modified == before.st_mtime &&
            found->second.inode == before.st_ino;
        const bool opened = reuse ? document.open_verified(full, found->second.identity, error) : document.open(full, error);
        if (!opened) { identities.erase(path); return false; }
        if (controlled_storage && stat_ok && stat(full.c_str(), &after) == 0 &&
            before.st_size == after.st_size && before.st_mtime == after.st_mtime && before.st_ino == after.st_ino) {
            if (identities.size() >= 8 && found == identities.end()) identities.erase(identities.begin());
            identities[path] = {after.st_size, after.st_mtime, after.st_ino, document.metadata()};
        }
        return true;
    }
    graphics::Canvas canvas;
    graphics::Font ui, body;
    Display display{*this};
    Session session{display};
    InputRouter router;
    EpubDocument document;
    Chapter chapter;
    graphics::Page page;
    std::optional<graphics::Canvas> body_canvas;
    graphics::LayoutSpec body_spec;
    std::vector<Hit> body_links;
    void clear_body_cache() { body_canvas.reset(); body_links.clear(); }
    static bool same_layout(const graphics::LayoutSpec& a, const graphics::LayoutSpec& b) {
        return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height &&
               a.font_px == b.font_px && a.line_height == b.line_height &&
               a.indent_chars == b.indent_chars && a.paragraph_gap == b.paragraph_gap;
    }
    static bool body_cache_room() {
#ifdef ESP_PLATFORM
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 2U * 1024 * 1024 &&
               heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 416U * 1024;
#else
        return true;
#endif
    }
    std::optional<graphics::Page> next_page;
    std::optional<Chapter> next_chapter;
    std::size_t next_chapter_index = 0;
    std::unique_ptr<graphics::Canvas> next_bitmap;
    std::unique_ptr<ContentJob> raster_job;
    std::uint64_t prefetch_generation = 0, bitmap_hits = 0;
    void poll_raster() {
        if (!raster_job || !raster_job->done()) return;
        auto job = std::move(raster_job);
        if (job->request.generation != prefetch_generation || job->request.identity.book_id != document.metadata().book_id) return;
        if (job->request.prefetch && job->request.kind == ContentJob::Kind::Chapter) {
            if (job->result.error.empty() && job->result.image) {
                next_chapter = std::move(job->result.chapter); next_chapter_index = job->request.chapter;
                next_page = std::move(job->result.page); next_bitmap = std::move(job->result.image);
            }
            return;
        }
        if (next_page && job->request.offset == next_page->start) {
            if (job->result.error.empty() && job->result.image) next_bitmap = std::move(job->result.image);
            else { raster_disabled = true; prefetch_done = false; }
        }
    }
    void stop_raster() { clear_prefetch(); raster_job.reset(); }
    graphics::LayoutSpec next_spec;
    std::size_t warm_cursor = 0;
    bool prefetch_done = false, raster_disabled = false;
    void clear_prefetch() {
        ++prefetch_generation;
        if (raster_job) raster_job->cancel();
        next_bitmap.reset();
        next_page.reset(); next_chapter.reset();
        warm_cursor = 0;
        prefetch_done = raster_disabled = false;
    }
    bool prefetch_due() const {
        return view == View::Reading && has_book() && card_available && !usb_prepared &&
               !display.busy() && !deferred && !save_job && !foreground_job && !pending_content && !prefetch_done;
    }
    void prepare_next(const std::function<bool()>& stop) {
        if ((stop && stop()) || raster_job) return;
        const StageTimer timer{"next_page_prepare"};
        if (!next_page) {
            const auto start = readable_offset(page.end);
            if (start >= chapter.text.size()) {
                if (!async_content) { prefetch_done = true; return; }
                if (counting_job) return;
                auto index = chapter_index;
                auto part = chapter.part + 1;
                if (chapter.last_part) {
                    part = 0; ++index;
                    while (index < document.spine().size() && !document.spine()[index].linear) ++index;
                }
                if (index >= document.spine().size()) { prefetch_done = true; return; }
#ifdef ESP_PLATFORM
                if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 5U * 1024 * 1024 ||
                    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 2U * 1024 * 1024) {
                    prefetch_done = true; return;
                }
#endif
                ContentJob::Request request;
                request.kind = ContentJob::Kind::Chapter; request.target = ContentJob::Target::Start;
                request.root = root; request.path = root + "/" + book_path; request.identity = document.metadata();
                request.font = body_font_path(); request.layout = next_spec = layout(settings, link_return ? 144 : 64);
                request.chapter = index; request.part = part; request.generation = prefetch_generation; request.prefetch = true;
                raster_job = std::make_unique<ContentJob>(std::move(request));
                if (!raster_job->start()) raster_job.reset();
                prefetch_done = true; return;
            }
            if (!async_content && image_at(start)) { prefetch_done = true; return; }
#ifdef ESP_PLATFORM
            if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 2048U * 1024 ||
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 256U * 1024) {
                prefetch_done = true;
                return;
            }
#endif
            next_spec = layout(settings, link_return ? 144 : 64);
            auto planned = plan_page(chapter, start, next_spec, true, stop);
            if (planned.error == "Layout cancelled") return;
            const auto bytes = planned.glyphs.capacity() * sizeof(graphics::PositionedGlyph) +
                               planned.lines.capacity() * sizeof(graphics::TextLine);
            if (!planned.error.empty() || planned.end <= planned.start || bytes > 256U * 1024) {
                prefetch_done = true;
                return;
            }
            next_page = std::move(planned);
            return;
        }
#ifdef ESP_PLATFORM
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 2048U * 1024 ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 64U * 1024) {
            next_page.reset(); prefetch_done = true; return;
        }
#endif
        if (async_content && !raster_disabled && !counting_job) {
            ContentJob::Request request;
            request.kind = ContentJob::Kind::RasterPage; request.font = body_font_path();
            request.identity = document.metadata(); request.layout = next_spec; request.page = *next_page;
            request.offset = next_page->start; request.generation = prefetch_generation; request.prefetch = true;
            if (const auto* image = image_at(next_page->start)) {
                request.kind = ContentJob::Kind::Image; request.root = root; request.path = root + "/" + book_path;
                request.image_resource = image->resource;
                request.cache_name = image_cache_key(request.identity.book_id, image->resource, request.layout);
            }
            raster_job = std::make_unique<ContentJob>(std::move(request));
            if (raster_job->start()) { prefetch_done = true; return; }
            raster_job.reset(); raster_disabled = true;
        }
        if (!body.size(next_page->font_px)) { next_page.reset(); prefetch_done = true; return; }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(15);
        while (warm_cursor < next_page->glyphs.size()) {
            if ((stop && stop()) || std::chrono::steady_clock::now() >= deadline) return;
            if (!body.warm(next_page->glyphs[warm_cursor].scalar)) {
                next_page.reset(); prefetch_done = true; return;
            }
            ++warm_cursor;
        }
        prefetch_done = true;
    }
    ReaderSettings settings, draft;
    SettingsSnapshot edit_snapshot;
    View view = View::Library;
    bool usb_prepared = false, usb_active = false, usb_confirm = false;
    UsbTransferAction usb_action = UsbTransferAction::None;
    PortalAction portal_action = PortalAction::None;
    PortalStatus portal;
    bool portal_closing = false;
    std::optional<Frame> pending;
    std::optional<NetworkOperation> network_request;
    std::shared_ptr<std::atomic_bool> network_cancel;
    std::optional<Anchor> remote_anchor;
    std::optional<ReadingRecord> upload_record, acknowledged_record, uncertain_record;
    sync::Config acknowledged_config, uncertain_config;
    std::string server_state = "未检查", remote_state = "未检查";
    sync::Config sync_config, badge_config;
    bool badge_configured = false, tools_save_pending = false;
    void refresh_badge_config() {
        std::string detail;
        badge_configured = sync::load_config(root, badge_config, detail);
    }
    std::uint64_t network_sequence = 0, active_network = 0;
    bool sync_checked = false, uploading = false;
    std::string sync_status;
    std::vector<Entry> entries;
    struct CoverThumbnail {
        std::string path;
        off_t size;
        time_t modified;
        std::vector<std::uint8_t> pixels;
    };
    std::vector<CoverThumbnail> covers;
    std::unique_ptr<ContentJob> cover_job;
    std::optional<ContentJob::Request> pending_covers;
    std::vector<ContentJob::CoverSource> missing_covers;
    std::uint64_t cover_generation = 0;
    void cancel_covers() {
        ++cover_generation; pending_covers.reset(); missing_covers.clear();
        if (cover_job) cover_job->cancel();
    }
    void stop_covers() { cancel_covers(); cover_job.reset(); }
    void poll_covers() {
        if (view != View::Library) cancel_covers();
        if (cover_job) {
            if (!cover_job->done()) return;
            auto job = std::move(cover_job);
            if (view == View::Library && job->request.generation == cover_generation &&
                job->result.covers.size() == job->request.covers.size()) {
                if (display.busy()) { cover_job = std::move(job); return; }
                for (std::size_t i = 0; i < job->request.covers.size(); ++i) {
                    const auto& source = job->request.covers[i];
                    if (covers.size() == 12) covers.erase(covers.begin());
                    covers.push_back({source.path, source.size, source.modified, std::move(job->result.covers[i])});
                }
                draw_library(); overlay();
            }
        }
        if (!pending_covers || display.busy() || save_job || foreground_job || pending_content || counting_job || raster_job) return;
        cover_job = std::make_unique<ContentJob>(std::move(*pending_covers)); pending_covers.reset();
        if (!cover_job->start()) {
            cover_job.reset(); message = "封面暂未生成，可继续阅读";
            for (const auto& source : missing_covers) {
                if (covers.size() == 12) covers.erase(covers.begin());
                covers.push_back({source.path, source.size, source.modified, {}});
            }
            draw_library(); overlay();
        }
    }
    std::vector<Hit> hits, links;
    std::optional<Anchor> link_return, confirmed_link_return;
    // Recent page spans are isolated by spine item, part and return-bar geometry.
    struct PageBoundary {
        std::size_t chapter, start, end;
        std::uint32_t part;
        bool return_bar;
    };
    std::vector<PageBoundary> page_history, computed_pages;
    void clear_page_history() { page_history.clear(); computed_pages.clear(); }
    void record_boundary(std::vector<PageBoundary>& entries, std::size_t start, std::size_t end) {
        if (entries.size() == 64) entries.erase(entries.begin());
        entries.push_back({chapter_index, start, readable_offset(end), chapter.part, link_return.has_value()});
    }
    std::size_t chapter_index = 0, offset = 0, library_page = 0, toc_page = 0;
    std::vector<std::size_t> toc_starts, toc_visible;
    std::set<std::size_t> toc_collapsed;
    bool toc_locate_current = false, library_grid = false, library_descending = false;
    bool footer_chapters = false, refresh_from_settings = false, import_from_settings = false;
    std::string detail_path, detail_position, detail_error;
    BookSnapshot management_snapshot;
    bool delete_original = false, drawing_error = false;
    DocumentMetadata detail_metadata;
    ProgressResult detail_progress;
    std::optional<std::uint64_t> detail_size;
    std::optional<int> battery_percent;
    std::uint64_t ui_sequence = 0;
    int parameter = FontSize;
    bool book_scope = true, card_available = true, font_fallback = false, builtin_body = false,
         advanced = false, reset_overrides = false;
    unsigned body_updates = 0;
    std::string effective_font, last_recorded;
    std::vector<std::string> fonts;
    std::function<void()> deferred;
    bool save_failed = false;
    // The most recent accepted input turned a page; see AppInput::queued.
    bool turned = false;
    // Whole-book page counting. chapter_pages[i][p] holds the page starts of part p of spine item
    // i, laid out from the beginning of the part under the applied typography; counted[i] marks
    // an item whose parts are all present. Non-linear items are outside the reading order and
    // count as finished without pages.
    static constexpr std::size_t not_counting = static_cast<std::size_t>(-1);
    static constexpr std::size_t max_pages_bytes = kPageIndexBytes;
    PageIndices chapter_pages;
    std::vector<bool> counted;
    // Only indices bound to verified content, font and layout identities serve navigation.
    std::vector<bool> counted_live;
    std::string pages_name, pages_header, font_identity;
    std::string body_font_path() const {
        return builtin_body ? "" : root + "/PicoPage/" +
            (font_fallback ? "fonts/LXGWWenKai-Regular.ttf" : settings.font);
    }
    // The synchronous fallback releases its extra chapter on input. The worker owns a separate
    // chapter until it observes cancellation; both paths retain accepted pagination offsets.
    Chapter counting_chapter;
    bool async_content = false;
    std::uint64_t counting_generation = 0;
    std::unique_ptr<ContentJob> counting_job;
    std::unique_ptr<ContentJob> foreground_job;
    std::optional<ContentJob::Request> pending_content;
    std::unique_ptr<graphics::Canvas> prepared_image;
    std::uint64_t foreground_generation = 0;
    bool content_loading = false, image_prepared = false;
    std::string prepared_image_error;
    void abandon_content() {
        cancel_covers();
        ++foreground_generation;
        if (foreground_job) foreground_job->cancel();
        pending_content.reset();
        content_loading = image_prepared = false;
        prepared_image.reset();
    }
    bool begin_image(std::size_t start, const DocumentImage& image) {
        abandon_content();
        release_counting();
        ContentJob::Request request;
        request.kind = ContentJob::Kind::Image;
        request.root = root; request.path = root + "/" + book_path; request.identity = document.metadata();
        request.layout = layout(settings, link_return ? 144 : 64);
        request.image_resource = image.resource;
        request.cache_name = image_cache_key(request.identity.book_id, image.resource, request.layout);
        request.generation = foreground_generation; request.chapter = chapter_index;
        request.part = chapter.part; request.offset = start;
        pending_content = std::move(request);
        content_loading = true;
        view = View::Loading;
        clear_body_cache(); clear_prefetch(); covers.clear();
        canvas.fill_rect(0, footer_top, 684, 88, 15);
        text("正在准备图片…", 36, 1156, 24, 400, 3);
        button(Dismiss, "取消", 488, 1140, 160, 64, false, 24);
        overlay();
        return true;
    }
    bool begin_open(const std::string& path) {
        cancel_sync();
        ContentJob::Request request;
        request.kind = ContentJob::Kind::Open;
        request.root = root; request.relative_path = path; request.path = root + "/" + path;
        request.content_revision = content_revision; request.generation = foreground_generation;
        struct stat info{};
        const auto found = identities.find(path);
        if (controlled_storage && found != identities.end() && stat(request.path.c_str(), &info) == 0 &&
            found->second.size == info.st_size && found->second.modified == info.st_mtime && found->second.inode == info.st_ino)
            request.identity = found->second.identity;
        pending_content = std::move(request);
        content_loading = true;
        view = View::Loading;
        clear_body_cache(); clear_prefetch(); covers.clear(); release(chapter); page = {};
        canvas.fill_rect(0, footer_top, 684, 88, 15);
        text("正在准备书籍…", 36, 1156, 24, 400, 3);
        button(Dismiss, "取消", 488, 1140, 160, 64, false, 24);
        overlay();
        return true;
    }
    bool begin_chapter(std::size_t index, std::uint32_t part, ContentJob::Target target,
                       std::size_t position = 0, const Anchor& anchor = {}, const std::string& fragment = "") {
        poll_raster();
        if (target == ContentJob::Target::Start && next_chapter && next_page && next_bitmap &&
            next_chapter_index == index && next_chapter->part == part && same_layout(next_spec, layout(settings, link_return ? 144 : 64))) {
            release_counting(); clear_body_cache();
            chapter = std::move(*next_chapter); next_chapter.reset(); chapter_index = index; offset = next_page->start;
            return show_page(offset);
        }
        abandon_content();
        ++counting_generation;
        release_counting();
        ContentJob::Request request;
        request.kind = ContentJob::Kind::Chapter; request.target = target;
        request.root = root; request.path = root + "/" + book_path; request.identity = document.metadata();
        request.font = body_font_path(); request.layout = layout(settings, link_return ? 144 : 64);
        request.chapter = index; request.part = part; request.offset = position;
        request.anchor = anchor; request.fragment = fragment; request.generation = foreground_generation;
        if (target == ContentJob::Target::End && !link_return && index < counted_live.size() && counted_live[index]) {
            const auto& parts = chapter_pages[index];
            const auto selected = part == kLastChapterPart && !parts.empty() ? parts.size() - 1 : part;
            if (selected < parts.size() && !parts[selected].empty()) request.known_start = parts[selected].back();
        }
        pending_content = std::move(request); content_loading = true; view = View::Loading;
        clear_body_cache(); clear_prefetch(); covers.clear(); release(chapter); page = {};
        canvas.fill_rect(0, footer_top, 684, 88, 15);
        text("正在准备章节…", 36, 1156, 24, 400, 3);
        button(Dismiss, "取消", 488, 1140, 160, 64, false, 24);
        overlay();
        return true;
    }
    bool begin_seek(const Anchor& anchor) {
        std::size_t index;
        if (!document.chapter_index_for(anchor.resource, index)) { error("目标位置不在正文中"); return false; }
        return begin_chapter(index, 0, ContentJob::Target::Anchor, 0, anchor);
    }
    void poll_content() {
        if (foreground_job && (!content_loading || foreground_job->request.generation != foreground_generation)) {
            if (foreground_job->started() && !foreground_job->done()) return;
            foreground_job.reset();
        }
        if (!foreground_job && pending_content) {
            foreground_job = std::make_unique<ContentJob>(std::move(*pending_content));
            pending_content.reset();
        }
        if (!foreground_job) return;
        if (!foreground_job->started()) {
            if (!content_loading) { foreground_job.reset(); return; }
            if (display.busy() || counting_job || raster_job || save_job || cover_job) return;
            if (!foreground_job->start()) {
                foreground_job.reset(); content_loading = false;
                error("内容任务无法启动，请返回后重试。");
            }
            return;
        }
        if (!foreground_job->done() || display.busy()) return;
        auto job = std::move(foreground_job);
        const auto& request = job->request;
        if (!content_loading || request.generation != foreground_generation) return;
        if (request.kind == ContentJob::Kind::Open) {
            content_loading = false;
            if (!job->result.error.empty()) { error("无法打开 EPUB：" + job->result.error); return; }
            document = std::move(*job->result.document);
            if (controlled_storage) {
                if (identities.size() >= 8 && !identities.count(request.relative_path)) identities.erase(identities.begin());
                identities[request.relative_path] = {job->result.file_size, job->result.file_modified,
                    job->result.file_inode, document.metadata()};
            }
            finish_open(request.relative_path, &job->result);
            return;
        }
        if (request.kind == ContentJob::Kind::Chapter) {
            content_loading = false;
            if (!job->result.error.empty()) { error(chapter_error(job->result.error)); return; }
            chapter = std::move(job->result.chapter); chapter_index = request.chapter;
            offset = job->result.offset;
            for (const auto& span : job->result.history) record_boundary(computed_pages, span.first, span.second);
            show_page(offset);
            return;
        }
        if (request.chapter != chapter_index || request.part != chapter.part) return;
        content_loading = false;
        image_prepared = true;
        prepared_image = std::move(job->result.image);
        prepared_image_error = job->result.error;
        show_page(request.offset);
        image_prepared = false;
        prepared_image.reset();
    }

    void stop_counting_job() {
        if (counting_job) { counting_job->cancel(); counting_job.reset(); }
    }
    void poll_counting_job() {
        if (!counting_job || !counting_job->done()) return;
        auto job = std::move(counting_job);
        const auto& request = job->request;
        const auto& result = job->result;
        if (request.generation != counting_generation) return;
        if (request.kind == ContentJob::Kind::PageCacheRead) {
            if (result.cache_finished) pages_cache_pending = false;
            if (job->result.page_cache) adopt_pages(std::move(*job->result.page_cache));
            return;
        }
        if (request.kind == ContentJob::Kind::PageCacheWrite) {
            if (result.cache_finished) pages_dirty = false;
            return;
        }
        if (request.kind == ContentJob::Kind::FontIdentity) {
            if (!result.error.empty()) pages_failed = true;
            else if (!result.font_identity.empty()) {
                font_identity = result.font_identity;
                reset_pages(true);
            }
            return;
        }
        if (request.chapter != counting_index ||
            request.part != counting_part || request.offset != counting_offset) return;
        if (!result.error.empty()) {
            if (memory_error(result.error)) counting_blocked = chapter_index;
            else pages_failed = true;
            return;
        }
        auto& starts = chapter_pages[counting_index][counting_part];
        if (!reserve_page_starts(starts, result.starts.size())) return;
        starts.insert(starts.end(), result.starts.begin(), result.starts.end());
        counting_offset = result.offset;
        if (result.part_done) {
            if (result.last_part) finish_counting();
            else { ++counting_part; counting_offset = 0; add_counting_part(); }
        }
    }
    std::size_t counting_index = not_counting, counting_offset = 0;
    std::uint32_t counting_part = 0;
    // The visible chapter during which memory was too low to load another one for counting.
    std::size_t counting_blocked = not_counting;
    bool counting_loaded = false;
    bool pages_complete = false, pages_failed = false, pages_dirty = false, pages_cache_pending = false;

    Impl(std::string card, std::string id)
        : root(std::move(card)), writer(std::move(id)), store(root) {}
    bool start(const Frame &frame) override {
        pending = frame;
        return true;
    }
    bool has_book() const {
        return !book_path.empty();
    }
    std::unique_ptr<ContentJob> save_job;
    std::optional<SaveRequest> saving_request;
    bool save_error_pending = false, recent_save_failed = false;
    void poll_save() {
        if (!save_job) return;
        if (!save_job->started()) {
            if (counting_job || raster_job || cover_job || (foreground_job && foreground_job->started() && !foreground_job->done())) return;
            if (save_job->start()) return;
            save_job->result.progress.status = StoreStatus::IoError;
        } else if (!save_job->done()) return;
        auto job = std::move(save_job);
        const auto request = std::exchange(saving_request, std::nullopt);
        const bool okay = job->result.progress.status == StoreStatus::Ok &&
            job->result.progress.record && *job->result.progress.record == *job->request.record;
        if (request) session.saved(*request, okay);
        save_failed = !okay;
        save_error_pending = !okay;
        if (!okay) message = "进度未保存，请检查 TF 卡后重试";
        if (job->result.last_book_saved) last_recorded = job->request.relative_path;
        else if (okay && job->request.save_last_book) {
            recent_save_failed = true; message = "最近阅读记录未保存";
        }
    }
    void drain_save() {
        if (!save_job) return;
        stop_covers(); stop_raster(); stop_counting_job();
        if (foreground_job && foreground_job->started()) foreground_job->wait();
        poll_save();
        if (save_job) { save_job->wait(); poll_save(); }
    }
    void schedule_save() {
        if (!async_content) { if (!persist()) error(message); return; }
        if (save_job) return;
        if (cover_job) {
            pending_covers = cover_job->request;
            pending_covers->generation = ++cover_generation;
            cover_job->cancel();
        }
        saving_request = session.request_save();
        if (!saving_request && (!session.visible_record() || last_recorded == book_path)) return;
        ContentJob::Request request;
        request.kind = ContentJob::Kind::SaveProgress; request.root = root; request.relative_path = book_path;
        request.record = saving_request ? saving_request->record : *session.visible_record(); request.save_last_book = last_recorded != book_path;
        save_job = std::make_unique<ContentJob>(std::move(request));
        if (counting_job) counting_job->cancel();
        if (raster_job) raster_job->cancel();
        poll_save();
    }
    bool persist() {
        drain_save();
        save_error_pending = false;
        auto request = session.request_save();
        if (!request)
            return !session.needs_save();
        const StageTimer timer{"progress_commit"};
        const auto result = store.save_progress(book_path, request->record);
        const bool okay =
            result.status == StoreStatus::Ok && result.record && *result.record == request->record;
        session.saved(*request, okay);
        save_failed = !okay;
        if (!okay)
            message = "进度未保存，请检查 TF 卡后重试";
        return okay;
    }
    void text(const std::string &value, int x, int top, int size = 28, int max_width = 612,
              std::uint8_t gray = 1, bool bold = false) {
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
                ui.draw(canvas, U'…', x + used, baseline, gray, bold);
                break;
            }
            ui.draw(canvas, scalars[i], x + used, baseline, gray, bold);
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
    int text_width(const std::string &value, int size) {
        ui.size(size);
        int length = 0;
        for (auto c : decode_utf8(value))
            length += ui.advance(c);
        return length;
    }
    void center(const std::string &value, int x, int top, int w, int size = 28,
                std::uint8_t gray = 1, bool bold = false) {
        text(value, x + std::max(0, (w - text_width(value, size)) / 2), top, size, w, gray, bold);
    }
    /**
     * One list row: title, optional description and a right-aligned value. A non-zero id makes
     * the row a control and appends a chevron to the value.
     */
    void value_row(int id, const std::string &title, const std::string &description,
                   const std::string &value, int y, int height = 104, bool bold_value = false) {
        const auto shown = !id ? value : value.empty() ? "›" : value + " ›";
        const int value_width = std::min(text_width(shown, 24), 400);
        const int title_width = 612 - value_width - 16;
        if (description.empty())
            text(title, 36, y + (height - 26) / 2 - 6, 26, title_width);
        else {
            text(title, 36, y + height / 2 - 42, 26, title_width);
            text(description, 36, y + height / 2 + 4, 20, title_width, 3);
        }
        text(shown, 648 - value_width, y + (height - 24) / 2 - 6, 24, value_width, 1, bold_value);
        canvas.line(36, y + height - 1, 647, y + height - 1, 12, 2);
        if (id) hits.push_back({24, y, 636, height, id});
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
    /** Page header: a back cell naming the parent page, when back is non-zero, and the title. */
    void header(const std::string &title, int back = Back, const std::string &parent = "返回") {
        canvas.fill_rect(0, 0, 684, 104, 15);
        int left = 28;
        if (back) {
            text("‹ " + parent, 28, 32, 28, 104);
            canvas.line(140, 0, 140, 103, 1, 2);
            hits.push_back({0, 0, 140, 104, back});
            left = 168;
        }
        text(title, left, 30, 32, 660 - left, 1, true);
        canvas.line(0, 103, 683, 103, 1, 3);
    }
    void dashed(int x, int y, int w, int h) {
        for (int i = 0; i < w; i += 12) {
            canvas.fill_rect(x + i, y, std::min(6, w - i), 2, 5);
            canvas.fill_rect(x + i, y + h - 2, std::min(6, w - i), 2, 5);
        }
        for (int i = 0; i < h; i += 12) {
            canvas.fill_rect(x, y + i, 2, std::min(6, h - i), 5);
            canvas.fill_rect(x + w - 2, y + i, 2, std::min(6, h - i), 5);
        }
    }
    void disabled_button(const std::string &label, int x, int y, int w, int h = 88) {
        dashed(x, y, w, h);
        center(label, x, y + (h - 26) / 2 - 3, w, 26, 5);
    }
    void pager(int previous, int next, std::size_t index, std::size_t total) {
        canvas.fill_rect(0, 1096, 684, 120, 15);
        canvas.line(0, 1096, 683, 1096, 1, 2);
        if (index > 0) button(previous, "‹ 上一屏", 24, 1112, 180, 88, false, 24);
        else disabled_button("‹ 上一屏", 24, 1112, 180);
        center(std::to_string(index + 1) + " / " + std::to_string(std::max<std::size_t>(1, total)),
               216, 1138, 252, 26);
        if (index + 1 < total) button(next, "下一屏 ›", 480, 1112, 180, 88, false, 24);
        else disabled_button("下一屏 ›", 480, 1112, 180);
    }
    void title_lines(const std::string &value, int x, int y, int width, int size, bool bold = false, int max_rows = 2) {
        ui.size(size);
        const auto scalars = decode_utf8(value);
        int used = 0, row = 0;
        const int baseline = y + ui.metrics().ascender;
        for (std::size_t i = 0; i < scalars.size(); ++i) {
            const int advance = ui.advance(scalars[i]);
            if (used + advance > width) { ++row; used = 0; }
            if (row == max_rows - 1 && i + 1 < scalars.size() && used + advance + ui.advance(U'…') > width) {
                ui.draw(canvas, U'…', x + used, baseline + row * (size + 8), 1, bold);
                break;
            }
            if (row >= max_rows) break;
            ui.draw(canvas, scalars[i], x + used, baseline + row * (size + 8), 1, bold);
            used += advance;
        }
    }
    bool typography_changed() const {
        const auto &applied = edit_snapshot.config.settings;
        return reset_overrides || draft.font != applied.font || draft.font_size_px != applied.font_size_px ||
            draft.line_height != applied.line_height || draft.margin_x_px != applied.margin_x_px ||
            draft.paragraph_gap_lines != applied.paragraph_gap_lines || draft.indent_chars != applied.indent_chars;
    }
    std::string chapter_label() const {
        for (const auto &item : document.toc())
            if (item.resource == chapter.resource) return item.label;
        return "第 " + std::to_string(chapter_index + 1) + " 章";
    }
    std::uint32_t total_pages() const {
        std::size_t total = 0;
        for (const auto &item : chapter_pages)
            for (const auto &starts : item) total += starts.size();
        return static_cast<std::uint32_t>(total);
    }
    /**
     * 1-based number of the counted page that holds the first visible character; 0 until every
     * chapter is counted. A page reached from a restored or linked position may span two counted
     * pages, so consecutive turns can repeat or skip a number.
     */
    std::uint32_t current_page() const {
        return current_page(page.start);
    }
    std::uint32_t current_page(std::size_t start) const {
        const auto &spine = document.spine();
        if (!pages_complete || chapter_index >= spine.size() || !spine[chapter_index].linear) return 0;
        const auto &parts = chapter_pages[chapter_index];
        if (chapter.part >= parts.size() || parts[chapter.part].empty()) return 0;
        std::size_t before = 0;
        for (std::size_t i = 0; i < chapter_index; ++i)
            for (const auto &starts : chapter_pages[i]) before += starts.size();
        for (std::uint32_t part = 0; part < chapter.part; ++part) before += parts[part].size();
        const auto &starts = parts[chapter.part];
        const auto within = std::upper_bound(starts.begin(), starts.end(), static_cast<std::uint32_t>(start)) - starts.begin();
        return static_cast<std::uint32_t>(before + std::max<std::ptrdiff_t>(1, within));
    }
    /** Position weighted by the uncompressed size of each linear chapter; used until pages are counted. */
    int estimated_percent(std::size_t end) const {
        const auto &spine = document.spine();
        std::uint64_t read = 0, total = 0;
        for (std::size_t i = 0; i < spine.size(); ++i) {
            if (!spine[i].linear) continue;
            total += spine[i].bytes;
            if (i < chapter_index) read += spine[i].bytes;
            else if (i == chapter_index && !chapter.text.empty()) {
                // The number of parts is unknown before the item is counted; one more is assumed.
                const std::uint64_t parts = chapter.part + (chapter.last_part ? 1 : 2);
                const std::uint64_t within = chapter.part * chapter.text.size() + std::min(end, chapter.text.size());
                read += spine[i].bytes * within / (parts * chapter.text.size());
            }
        }
        return total ? static_cast<int>(read * 100 / total) : 0;
    }
    /** Whole-book percentage of a page spanning [start, end) of the loaded chapter. */
    std::string book_percent(std::size_t start, std::size_t end) const {
        if (const auto current = current_page(start))
            return std::to_string(static_cast<std::uint64_t>(current) * 100 / total_pages()) + "%";
        return "约 " + std::to_string(estimated_percent(end)) + "%";
    }
    std::string book_percent() const {
        return book_percent(page.start, page.end);
    }
    /** Page number, page total and percentage; the percentage alone until every chapter is counted. */
    std::string book_position(std::size_t start, std::size_t end) const {
        if (const auto current = current_page(start))
            return "第 " + std::to_string(current) + " / " + std::to_string(total_pages()) + " 页 · " + book_percent(start, end);
        if (pages_complete) return "共 " + std::to_string(total_pages()) + " 页 · " + book_percent(start, end);
        return book_percent(start, end);
    }
    std::string book_progress() const {
        const auto position = book_position(page.start, page.end);
        return pages_complete || pages_failed ? position : position + " · 正在统计页数";
    }
    std::string sync_badge() const {
        if (session.needs_save()) return "尚未保存";
        if (active_network) return "同步中";
        if (remote_anchor) return "位置不同 ›";
        if (!badge_configured) return "未配置";
        const auto& config = badge_config;
        if (uncertain_record && uncertain_record->book_id == document.metadata().book_id && same_account(config, uncertain_config))
            return "待核实";
        if (acknowledged_record && session.saved_record() && *acknowledged_record == *session.saved_record() && same_account(config, acknowledged_config))
            return "已上传";
        return "待上传";
    }
    void reading_header() {
        canvas.fill_rect(0, 0, 684, 104, 15);
        text("‹ 书库", 28, 32, 28, 100);
        hits.push_back({0, 0, 128, 104, Library});
        text(chapter_label(), 132, 14, 30, 380, 1, true);
        text(document.metadata().title, 132, 60, 22, 212, 3);
        text("· " + book_percent(), 352, 60, 22, 176, 3);
        button(Sync, sync_badge(), 536, 26, 124, 54, session.needs_save() || remote_anchor.has_value(), 22);
        canvas.line(0, 103, 683, 103, 1, 3);
    }
    void tabs(int top, int active) {
        const int ids[] = {Contents, Typography, Sync, Refresh, Back};
        const char *names[] = {"目录", "排版", "同步", "刷新", "收起"};
        canvas.fill_rect(0, top, 684, 100, 15);
        for (int i = 0; i < 5; ++i) {
            const int x = i * 143, width = i == 4 ? 112 : 143;
            if (ids[i] == active) canvas.fill_rect(x, top, width, 100, 1);
            canvas.line(x, top, x, top + 99, 1, 2);
            center(names[i], x, top + 32, width, 26, ids[i] == active ? 15 : 1, ids[i] == active);
            hits.push_back({x, top, width, 100, ids[i]});
        }
        canvas.line(0, top, 683, top, 1, 3);
        canvas.line(0, top + 99, 683, top + 99, 1, 2);
    }
    void tool_icon(int id, int cx, int cy) {
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
        } else outline(cx - 16, cy - 16, 32, 32, 2);
    }
    void note(const std::string &value, int y = 1020) {
        text(value, 36, y, 24, 612, 3);
    }
    void force_refresh() {
        const auto &visible = session.visible_record();
        display.present({{"ui", 0, 0, ++ui_sequence}, visible ? visible->anchor : Anchor{},
                         FramePurpose::ManualRefresh, RefreshIntent::FullClean});
    }
    void overlay(bool full = false) {
        display.present({{"ui", 0, 0, ++ui_sequence},
                         {},
                         FramePurpose::Overlay,
                         full ? RefreshIntent::FullClean : RefreshIntent::Content});
    }
    /**
     * Presents a status line on the current page and runs work when that frame has reached the
     * panel. complete_frame invokes work before returning, so no input is dispatched in between.
     */
    void defer(const std::string &status, std::function<void()> work) {
        if (display.busy())
            return;
        if (view == View::Reading) {
            canvas.fill_rect(0, footer_top, 684, 88, 15);
            text(status, 36, 1156, 24, 612, 3);
        } else {
            const int top = view == View::BookDetails ? 1056 : 1040;
            canvas.fill_rect(24, top, 636, 48, 15);
            note(status, top + 10);
        }
        deferred = std::move(work);
        overlay();
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
        if (effective_font == selected.font && !builtin_body && controlled_storage)
            return;
        clear_body_cache();
        clear_prefetch();
        font_identity.clear();
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
        refresh_badge_config();
        if (!ui.open(root + "/PicoPage/fonts/NotoSansSC-Regular.otf") && !ui.open_fallback())
            return false;
        settings = store.load_settings().settings;
        library_grid = settings.library_view == "grid";
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
        if (library_descending) std::reverse(entries.begin(), entries.end());
        library_page = std::min(library_page, entries.empty() ? 0 : (entries.size() - 1) / library_page_size());
    }
    std::size_t library_page_size() const { return library_grid ? 6 : 5; }
    static std::string entry_title(const Entry &entry) {
        return entry.directory ? entry.name : entry.name.substr(0, entry.name.size() - 5);
    }
    /** FNV-1a of a card-relative path; names the cache entries of one book. */
    static unsigned long long path_hash(const std::string &relative) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char c : relative) hash = (hash ^ c) * 1099511628211ULL;
        return hash;
    }
    struct CoverEntry {
        std::string name, header;
        std::size_t bytes;
    };
    /** Names the PicoPage/cache entry for one size of a cover; the header binds it to a file version. */
    static CoverEntry cover_entry(const std::string &relative, const struct stat &info, int width,
                                  int height, bool crop) {
        char name[64];
        std::snprintf(name, sizeof(name), "covers/%016llx-%dx%d%s", path_hash(relative),
                      width, height, crop ? "c" : "");
        return {name, "PPCOVER1\n" + relative + "\n" + std::to_string(info.st_size) + " " +
                          std::to_string(static_cast<long long>(info.st_mtime)) + "\n",
                static_cast<std::size_t>((width + 1) / 2) * height};
    }
    /** Loads cached packed pixels; an entry holding only its header records a book without a usable cover. */
    bool cached_cover(const CoverEntry &entry, std::vector<std::uint8_t> &pixels) {
        std::string cached;
        if (store.load_cache(entry.name, entry.header.size() + entry.bytes, cached) != StoreStatus::Ok ||
            cached.compare(0, entry.header.size(), entry.header) != 0 ||
            (cached.size() != entry.header.size() && cached.size() != entry.header.size() + entry.bytes))
            return false;
        pixels.assign(cached.begin() + static_cast<std::ptrdiff_t>(entry.header.size()), cached.end());
        return true;
    }
    /** Decodes the cover from the EPUB and caches the result, so each file version decodes once. */
    std::vector<std::uint8_t> decode_cover(const std::string &relative, const CoverEntry &entry,
                                           int width, int height, bool crop) {
        graphics::Canvas thumbnail(width, height);
        std::vector<std::uint8_t> encoded;
        std::string detail;
        const bool loaded = EpubDocument::read_cover(root + "/" + relative, encoded, detail) &&
            graphics::render_image(thumbnail, encoded, 0, 0, width, height, detail, crop);
        // An allocation failure is transient and must not be remembered as a missing cover.
        if (loaded || detail.find("allocation") == std::string::npos)
            store.save_cache(entry.name, loaded ? entry.header + std::string(thumbnail.pixels().begin(), thumbnail.pixels().end())
                                                : entry.header);
        return loaded ? thumbnail.pixels() : std::vector<std::uint8_t>{};
    }
    /** Packed width x height cover pixels of a book file, or empty when it has no usable cover. */
    std::vector<std::uint8_t> cover_pixels(const std::string &relative, const struct stat &info,
                                           int width, int height, bool crop) {
        const auto entry = cover_entry(relative, info, width, height, crop);
        std::vector<std::uint8_t> pixels;
        return cached_cover(entry, pixels) ? pixels : decode_cover(relative, entry, width, height, crop);
    }
    /** Copies packed cover pixels, as returned by cover_pixels, to the canvas. */
    void blit(const std::vector<std::uint8_t> &pixels, int x, int y, int width, int height) {
        const int stride = (width + 1) / 2;
        for (int row = 0; row < height; ++row)
            for (int column = 0; column < width; ++column) {
                const auto packed = pixels[row * stride + column / 2];
                canvas.pixel(x + column, y + row, column % 2 ? packed >> 4 : packed & 15);
            }
    }
    enum class Cover { Drawn, None, Undecoded };
    /** Draws a library thumbnail. Without decode, a cover absent from both caches is left Undecoded. */
    Cover draw_cover(const Entry &entry, int x, int y, bool decode) {
        struct stat info{};
        const auto path = root + "/" + entry.path;
        if (entry.directory || !safe_existing(root, entry.path) || stat(path.c_str(), &info) != 0)
            return Cover::None;
        auto found = std::find_if(covers.begin(), covers.end(), [&](const auto &cover) {
            return cover.path == entry.path;
        });
        if (found != covers.end() && (found->size != info.st_size || found->modified != info.st_mtime))
        {
            covers.erase(found);
            found = covers.end();
        }
        if (found == covers.end()) {
            const auto cached = cover_entry(entry.path, info, 164, 184, false);
            std::vector<std::uint8_t> pixels;
            if (!cached_cover(cached, pixels)) {
                if (!decode || async_content) {
                    if (async_content) missing_covers.push_back({entry.path, cached.name, cached.header, info.st_size, info.st_mtime});
                    return Cover::Undecoded;
                }
                pixels = decode_cover(entry.path, cached, 164, 184, false);
            }
            // Two visible screens occupy at most 181,056 bytes of packed thumbnail pixels.
            if (covers.size() == 12) covers.erase(covers.begin());
            covers.push_back({entry.path, info.st_size, info.st_mtime, std::move(pixels)});
            found = covers.end() - 1;
        }
        if (found->pixels.empty()) return Cover::None;
        blit(found->pixels, x, y, 164, 184);
        return Cover::Drawn;
    }
    /**
     * Draws the library. Covers that still need decoding first appear as titles with a status
     * line. Native asynchronous mode prepares a visible batch and merges its display update.
     */
    void draw_library(bool decode = false) {
        if (async_content) cancel_covers();
        decode = decode || display.busy();
        bool undecoded = false;
        view = View::Library;
        hits.clear();
        canvas.clear();
        text(folder.empty() ? "书库" : "‹ " + basename(folder), 36, 28, 34, 350, 1, true);
        if (!folder.empty()) hits.push_back({0, 0, 380, 104, Back});
        for (int x : {396, 540}) canvas.line(x, 0, x, 104, 1, 2);
        center("导入", 396, 32, 144, 26);
        center("设置", 540, 32, 144, 26);
        hits.push_back({396, 0, 144, 104, Import});
        hits.push_back({540, 0, 144, 104, Settings});
        canvas.line(0, 103, 683, 103, 1, 3);
        text("继续阅读", 36, 128, 22, 612, 3);
        outline(24, 172, 636, 174, 3);
        if (has_book() || !last_book.empty()) {
            const auto title = has_book() ? document.metadata().title : basename(last_book);
            title_lines(title, 48, 192, 454, 30, true);
            text(has_book() ? (document.metadata().author.empty() ? "" : document.metadata().author + " · ") + chapter_label() : "打开后恢复已保存的位置", 48, 292, 22, 450, 3);
            text("继续 ›", 550, 240, 26, 98);
            hits.push_back({24, 172, 636, 174, Tools});
        } else {
            text("从书库打开一本 EPUB", 48, 202, 30, 570, 1, true);
            text("阅读位置自动保存在存储卡", 48, 282, 22, 570, 3);
        }
        text("本机书籍 · " + std::to_string(entries.size()) + " 项", 36, 378, 22, 280, 3);
        text(library_descending ? "书名 ↓" : "书名 ↑", 340, 378, 22, 108);
        hits.push_back({324, 354, 144, 64, LibrarySort});
        button(LibraryList, "列表", 476, 358, 92, 60, !library_grid, 22);
        button(LibraryGrid, "卡片", 568, 358, 92, 60, library_grid, 22);
        canvas.line(0, 430, 683, 430, 1, 2);
        const auto size = library_page_size(), begin = library_page * size;
        for (std::size_t i = begin; i < std::min(begin + size, entries.size()); ++i) {
            const auto &entry = entries[i];
            const auto label = entry_title(entry);
            const auto status = entry.directory ? "文件夹 ›" : entry.path == last_book ? "最近阅读" : "EPUB";
            if (library_grid) {
                const int x = 24 + static_cast<int>((i - begin) % 3) * 216;
                const int y = 454 + static_cast<int>((i - begin) / 3) * 300;
                outline(x + 12, y, 168, 188);
                const auto cover = draw_cover(entry, x + 14, y + 2, decode);
                undecoded = undecoded || cover == Cover::Undecoded;
                if (cover != Cover::Drawn)
                    title_lines(label, x + 28, y + 42, 136, 24, true);
                title_lines(label, x + 4, y + 204, 194, 22);
                text(entry.directory ? status : "详情 ›", x + 4, y + 265, 22, 194, 3);
                if (!entry.directory) hits.push_back({x, y + 204, 204, 88, DetailBase + static_cast<int>(i)});
                hits.push_back({x, y, 204, entry.directory ? 292 : 204, EntryBase + static_cast<int>(i)});
            } else {
                const int y = 432 + static_cast<int>(i - begin) * 126;
                title_lines(label, 36, y + 12, 486, 28);
                text(entry.directory ? entry.path : "本地 EPUB", 36, y + 88, 22, 470, 3);
                text(entry.directory ? status : "详情 ›", 552, y + 44, 22, 108, 3);
                canvas.line(24, y + 125, 660, y + 125, 12, 2);
                if (!entry.directory) hits.push_back({548, y, 112, 126, DetailBase + static_cast<int>(i)});
                hits.push_back({24, y, entry.directory ? 636 : 524, 126, EntryBase + static_cast<int>(i)});
            }
        }
        if (entries.empty()) {
            text(card_available ? "书库里还没有书" : "存储卡不可用", 36, 500, 32, 612, 1, true);
            note(card_available ? "可通过导入，将 EPUB 复制到存储卡。" : "这不代表书籍已被删除。", 570);
        }
        if (undecoded || !message.empty()) {
            canvas.fill_rect(24, 1040, 636, 48, 15);
            note(undecoded ? "正在生成封面…" : message, 1050);
        }
        pager(Prev, Next, library_page, (entries.size() + size - 1) / size);
        if (undecoded && async_content) {
            ContentJob::Request request;
            request.kind = ContentJob::Kind::LibraryCovers; request.root = root;
            request.generation = cover_generation; request.covers = missing_covers;
            pending_covers = std::move(request);
        } else if (undecoded)
            deferred = [this] {
                if (view != View::Library) return;
                draw_library(true);
                overlay();
            };
    }
    bool open(std::string path) {
        if (display.busy() || !card_available) return false;
        abandon_content();
        ++counting_generation;
        if (counting_job) counting_job->cancel();
        refresh_badge_config();
        clear_body_cache();
        clear_prefetch();
        if (!safe_existing(root, path)) {
            error("书籍路径无效或包含符号链接");
            return false;
        }
        if (has_book() && !persist()) {
            error(message);
            return false;
        }
        save_pages();
        if (async_content) return begin_open(path);
        std::string detail;
        if (!open_document(path, detail)) {
            book_path.clear();
            session.open("", "", writer);
            error("无法打开 EPUB：" + detail);
            return false;
        }
        return finish_open(path);
    }
    bool finish_open(const std::string& path, ContentJob::Result* loaded = nullptr) {
        std::string detail;
        cancel_sync();
        link_return.reset();
        confirmed_link_return.reset();
        book_path = path;
        last_book = path;
        const auto configuration = loaded ? loaded->configuration : store.load_settings(path);
        settings = configuration.settings;
        load_fonts();
        clear_page_history();
        reset_pages();
        const auto restored =
            loaded ? loaded->progress : store.load_progress(path, document.metadata().book_id, content_revision);
        session.open(document.metadata().book_id, content_revision, writer, restored.record);
        if (restored.status != StoreStatus::Ok && restored.status != StoreStatus::Missing && restored.status != StoreStatus::Cleared) {
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
        if (restored.status != StoreStatus::Ok && restored.status != StoreStatus::Missing && restored.status != StoreStatus::Cleared)
            message = "原进度无法读取，未覆盖原记录";
        if (loaded) {
            chapter = std::move(loaded->chapter);
            chapter_index = loaded->chapter_index;
            offset = loaded->offset;
        } else if (restored.record) {
            const auto holds = [&](const Chapter &part) {
                std::uint32_t ignored;
                return part.offset_for(restored.record->anchor, ignored);
            };
            if (!document.chapter_index_for(restored.record->anchor.resource, chapter_index) ||
                !find_content(store, document, chapter_index, chapter, detail, ChapterLoad::Reading, holds) ||
                !chapter.offset_for(restored.record->anchor, restored_offset)) {
                error(memory_error(detail) ? chapter_error(detail) : "保存的位置无法映射，请保留进度文件并检查 EPUB");
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
    bool restore_confirmed_position() {
        const auto record =
            session.visible_record() ? session.visible_record() : session.saved_record();
        if (!record) {
            draw_library();
            overlay();
            return false;
        }
        std::uint32_t position;
        if (!seek(record->anchor, position, "已显示位置无法恢复"))
            return false;
        offset = position;
        return true;
    }
    bool recover_body() {
        const auto record = session.visible_record() ? session.visible_record() : session.saved_record();
        std::uint32_t position;
        if (async_content && record && !chapter.offset_for(record->anchor, position)) return begin_seek(record->anchor);
        return restore_confirmed_position() && show_page(offset);
    }
    std::uint32_t restored_offset = 0;
    static bool memory_error(const std::string &detail) {
        return detail.rfind(kChapterMemoryError, 0) == 0;
    }
    static std::string chapter_error(const std::string &detail) {
        return memory_error(detail) ? "本章内容过大，设备内存不足，无法显示。" : "章节读取失败：" + detail;
    }
    /**
     * Makes the part that load produces the chapter being read. The visible chapter stays in
     * memory during the load so that a failure leaves the page intact. When the parser stops for
     * lack of memory, the visible chapter is released and the load repeated; if it fails again,
     * the error page reloads the chapter of the last confirmed position. missing is the message
     * for a part that does not exist.
     */
    bool load_with(std::size_t index, const std::function<bool(Chapter &, std::string &)> &load,
                   const std::string &missing = "") {
        clear_body_cache();
        clear_prefetch();
        std::string detail;
        Chapter candidate;
        bool loaded = load(candidate, detail);
        if (!loaded && memory_error(detail) && !chapter.text.empty()) {
            release(chapter);
            loaded = load(candidate, detail);
        }
        if (!loaded) {
            error(!missing.empty() && detail.rfind("Chapter part is not available", 0) == 0 ? missing : chapter_error(detail));
            return false;
        }
        chapter = std::move(candidate);
        chapter_index = index;
        offset = 0;
        return true;
    }
    bool load_chapter(std::size_t index, std::uint32_t part = 0) {
        const StageTimer timer{"chapter_reading"};
        return load_with(index, [&](Chapter &loaded, std::string &detail) {
            return load_content(store, document, index, loaded, detail, ChapterLoad::Reading, part);
        });
    }
    /** Loads the part that holds anchor, unless it is the loaded one, and returns the offset in it. */
    bool seek(const Anchor &anchor, std::uint32_t &position, const std::string &missing) {
        std::size_t index;
        if (!document.chapter_index_for(anchor.resource, index)) {
            error(missing);
            return false;
        }
        if (index == chapter_index && chapter.resource == anchor.resource && chapter.offset_for(anchor, position))
            return true;
        const auto holds = [&](const Chapter &part) {
            std::uint32_t ignored;
            return part.offset_for(anchor, ignored);
        };
        return load_with(index, [&](Chapter &loaded, std::string &detail) {
            return find_content(store, document, index, loaded, detail, ChapterLoad::Reading, holds);
        }, missing) && chapter.offset_for(anchor, position);
    }
    /**
     * Loads the part of a spine item that holds the fragment, or its first part without one. A
     * fragment recorded at the end of a part marks text that begins the next part.
     */
    bool seek_fragment(std::size_t index, const std::string &fragment, std::uint32_t &position,
                       const std::string &missing) {
        position = 0;
        if (fragment.empty()) return load_chapter(index);
        const auto holds = [&](const Chapter &part) {
            std::uint32_t ignored;
            return part.fragment_offset(fragment, ignored);
        };
        if (!load_with(index, [&](Chapter &loaded, std::string &detail) {
                return find_content(store, document, index, loaded, detail, ChapterLoad::Reading, holds);
            }, missing) || !chapter.fragment_offset(fragment, position))
            return false;
        if (position >= chapter.text.size() && !chapter.last_part) {
            position = 0;
            return load_chapter(index, chapter.part + 1);
        }
        return true;
    }
    void footer() {
        canvas.fill_rect(0, footer_top, 684, 88, 15);
        const auto position = footer_chapters ? "第 " + std::to_string(chapter_index + 1) + " 章 / 共 " +
            std::to_string(document.spine().size()) + " 章" : book_progress();
        text(message.empty() ? position : message, 36, 1156, 24, 476, 3);
        outline(556, 1165, 32, 17, 2);
        canvas.fill_rect(588, 1170, 3, 7, 1);
        if (battery_percent) canvas.fill_rect(560, 1169, *battery_percent * 24 / 100, 9, 3);
        text(battery_percent ? std::to_string(*battery_percent) + "%" : "—", 600, 1156, 24, 80, 3);
    }
    std::size_t readable_offset(std::size_t start) const {
        return pico_page::readable_offset(chapter, start);
    }
    const DocumentImage* image_at(std::size_t start) const {
        return pico_page::image_at(chapter, start);
    }
    graphics::Page plan_page(std::size_t start, const graphics::LayoutSpec& spec) {
        return plan_page(chapter, start, spec);
    }
    graphics::Page plan_page(const Chapter &source, std::size_t start, const graphics::LayoutSpec& spec,
                             bool collect = true, const std::function<bool()>& stop = {}) {
        const StageTimer timer{"page_layout"};
        return plan_content(body, source, start, spec, collect, stop);
    }

    /**
     * Restarts page counting for the applied typography and adopts a matching cache entry.
     * The entry is bound to the book file, the body font file and the page geometry.
     */
    void reset_pages(bool keep_prepared_frames = false) {
        ++counting_generation;
        if (!keep_prepared_frames) { clear_body_cache(); clear_prefetch(); }
        const auto &spine = document.spine();
        chapter_pages.assign(spine.size(), {});
        counted.assign(spine.size(), false);
        counted_live.assign(spine.size(), false);
        for (std::size_t i = 0; i < spine.size(); ++i) counted[i] = !spine[i].linear;
        counting_index = counting_blocked = not_counting;
        release_counting();
        pages_failed = pages_dirty = pages_cache_pending = false;
        pages_complete = std::find(counted.begin(), counted.end(), false) == counted.end();
        pages_name.clear();
        if (builtin_body) font_identity = graphics::Font::fallback_identity();
        if (!async_content && font_identity.empty()) font_identity = sha256_file(body_font_path());
        if (!font_identity.empty()) load_pages_cache();
    }
    void load_pages_cache() {
        const auto& spine = document.spine();
        struct stat book {}, font {};
        const std::string font_file = builtin_body ? "" : font_fallback ? "fonts/LXGWWenKai-Regular.ttf" : settings.font;
        if (stat((root + "/" + book_path).c_str(), &book) != 0 ||
            (!font_file.empty() && stat((root + "/PicoPage/" + font_file).c_str(), &font) != 0)) {
            pages_failed = true;
            return;
        }
        const auto spec = layout(settings);
        char name[32], geometry[160];
        std::snprintf(name, sizeof(name), "pages/%016llx", path_hash(book_path));
        std::snprintf(geometry, sizeof(geometry), "%lld %lld\n%lld %lld\n%d %d %d %d %d %d %d %d %zu\n",
                      static_cast<long long>(book.st_size), static_cast<long long>(book.st_mtime),
                      static_cast<long long>(font.st_size), static_cast<long long>(font.st_mtime),
                      spec.left, spec.top, spec.width, spec.height, spec.font_px, spec.line_height,
                      spec.indent_chars, spec.paragraph_gap, spine.size());
        pages_name = name;
        pages_header = std::string("PPPAGES3\n") + content_revision + ":pagination-v1\n" + book_path + "\n" + font_file + "\n" + geometry +
            document.metadata().book_id + "\n" + font_identity + "\n";
        if (async_content) { pages_cache_pending = true; return; }
        if (auto cached = read_page_cache(store, pages_name, pages_header, spine)) adopt_pages(std::move(*cached));
    }
    void adopt_pages(PageCache cached) {
        chapter_pages = std::move(cached.pages); counted = std::move(cached.counted);
        counted_live = counted;
        pages_complete = std::find(counted.begin(), counted.end(), false) == counted.end();
    }

    std::size_t page_index_bytes() const {
        std::size_t bytes = chapter_pages.capacity() * sizeof(decltype(chapter_pages)::value_type);
        for (const auto& parts : chapter_pages) {
            bytes += parts.capacity() * sizeof(std::vector<std::uint32_t>);
            for (const auto& starts : parts) bytes += starts.capacity() * sizeof(std::uint32_t);
        }
        return bytes;
    }
    bool add_counting_part() {
        auto& parts = chapter_pages[counting_index];
        if (parts.size() == parts.capacity()) {
            const auto bytes = page_index_bytes();
            const auto unit = sizeof(std::vector<std::uint32_t>);
            if (bytes > max_pages_bytes || max_pages_bytes - bytes < unit) {
                pages_failed = true; release_counting(); return false;
            }
            const auto desired = std::max<std::size_t>(1, parts.capacity() * 2);
            parts.reserve(std::min(desired, parts.capacity() + (max_pages_bytes - bytes) / unit));
        }
        parts.emplace_back();
        return true;
    }
    bool reserve_page_starts(std::vector<std::uint32_t>& starts, std::size_t extra) {
        const auto bytes = page_index_bytes();
        const auto required = starts.size() + extra;
        const auto growth = required > starts.capacity() ? required - starts.capacity() : 0;
        if (bytes > max_pages_bytes || growth > (max_pages_bytes - bytes) / sizeof(std::uint32_t)) {
            pages_failed = true;
            release_counting();
            return false;
        }
        if (growth) {
            const auto spare = (max_pages_bytes - bytes) / sizeof(std::uint32_t);
            const auto desired = std::max<std::size_t>(required, std::max<std::size_t>(64, starts.capacity() * 2));
            starts.reserve(std::min(desired, starts.capacity() + spare));
        }
        return true;
    }
    /** Writes counted chapters to PicoPage/cache so that a later open resumes instead of recounting. */
    void save_pages() {
        if (async_content || !pages_dirty || pages_name.empty()) return;
        if (write_page_cache(store, pages_name, pages_header, chapter_pages, counted)) pages_dirty = false;
    }
    bool counting_due() const {
        return view == View::Reading && has_book() && card_available && !usb_prepared &&
               !display.busy() && !deferred && !save_job && !foreground_job && !pending_content && !raster_job && (!pages_complete || pages_cache_pending || pages_dirty) && !pages_failed &&
               counting_blocked != chapter_index;
    }
    /** Frees the chapter loaded for counting; its partial count resumes after a reload. */
    void release_counting() {
        if (counting_job) counting_job->cancel();
        release(counting_chapter);
        counting_loaded = false;
    }
    /**
     * True when a layout load of one part of a spine item with this many XHTML bytes fits in
     * memory with room to spare. The parser stops by itself when memory runs short; this check
     * avoids starting a parse that would. The text is one reserved block of four bytes per XHTML
     * byte, at most two part lengths, and the records, retained link and fragment strings and
     * parser buffers of one part stay below 1.5 MiB. The rest of the 2 MiB allowance is headroom.
     */
    static bool counting_memory(std::uint32_t bytes) {
#ifdef ESP_PLATFORM
        const std::size_t text = 4U * (std::min<std::uint32_t>(bytes, 2 * kChapterPartScalars) + 1024);
        return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= text + 64U * 1024 &&
               heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= text + 2048U * 1024;
#else
        static_cast<void>(bytes);
        return true;
#endif
    }
    void finish_counting() {
        counted[counting_index] = true;
        counted_live[counting_index] = true;
        counting_index = not_counting;
        release_counting();
        pages_dirty = true;
        pages_complete = std::find(counted.begin(), counted.end(), false) == counted.end();
        if (pages_complete && !async_content) save_pages();
    }
    /** Starts independent counting or advances the synchronous fallback under the same geometry. */
    void count_pages(const std::function<bool()>& stop) {
        const StageTimer timer{"count_slice"};
        if (stop && stop()) return;
        const auto &spine = document.spine();
        if (async_content && font_identity.empty()) {
            if (!counting_job) {
                ContentJob::Request request;
                request.font = body_font_path(); request.kind = ContentJob::Kind::FontIdentity;
                request.generation = counting_generation;
                counting_job = std::make_unique<ContentJob>(std::move(request));
                if (!counting_job->start()) { counting_job.reset(); counting_blocked = chapter_index; }
            }
            return;
        }
        if (async_content && (pages_cache_pending || pages_dirty)) {
            if (counting_job) return;
#ifdef ESP_PLATFORM
            if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 4U * 1024 * 1024 ||
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 2U * 1024 * 1024) {
                counting_blocked = chapter_index; return;
            }
#endif
            ContentJob::Request request;
            request.kind = pages_cache_pending ? ContentJob::Kind::PageCacheRead : ContentJob::Kind::PageCacheWrite;
            request.root = root; request.cache_name = pages_name; request.cache_header = pages_header;
            request.generation = counting_generation;
            if (pages_cache_pending) request.spine = spine;
            else request.page_cache = PageCache{chapter_pages, counted};
            counting_job = std::make_unique<ContentJob>(std::move(request));
            if (!counting_job->start()) { counting_job.reset(); counting_blocked = chapter_index; }
            return;
        }
        if (pages_complete) return;
        if (counting_index == not_counting) {
            auto next = static_cast<std::size_t>(std::find(counted.begin(), counted.end(), false) - counted.begin());
            if (chapter_index < spine.size() && !counted[chapter_index] && chapter.resource == spine[chapter_index].resource)
                next = chapter_index;
            counting_index = next;
            counting_part = 0;
            counting_offset = 0;
            chapter_pages[next].clear();
            if (!add_counting_part()) return;
        }
        if (async_content) {
            if (counting_job) return;
            if (!counting_memory(spine[counting_index].bytes)) { counting_blocked = chapter_index; return; }
            ContentJob::Request request;
            request.path = root + "/" + book_path;
            request.font = body_font_path();
            request.identity = document.metadata(); request.layout = layout(settings);
            request.generation = counting_generation; request.chapter = counting_index;
            request.part = counting_part; request.offset = counting_offset;
            counting_job = std::make_unique<ContentJob>(std::move(request));
            if (!counting_job->start()) { counting_job.reset(); counting_blocked = chapter_index; }
            return;
        }
        const bool in_place = counting_index == chapter_index && counting_part == chapter.part &&
                              chapter.resource == spine[counting_index].resource;
        if (in_place) release_counting();
        else if (!counting_loaded || counting_chapter.part != counting_part ||
                 counting_chapter.resource != spine[counting_index].resource) {
            release_counting();
            if (!counting_memory(spine[counting_index].bytes)) {
                counting_blocked = chapter_index;
                return;
            }
            std::string detail;
            const StageTimer load_timer{"chapter_counting"};
            if (load_content(store, document, counting_index, counting_chapter, detail, ChapterLoad::Layout, counting_part, stop))
                counting_loaded = true;
            else if (detail == kChapterCancelled)
                return;
            else if (memory_error(detail))
                counting_blocked = chapter_index;
            else if (detail.find("limit") != std::string::npos) {
                // An item beyond the parser limits cannot be shown either; it has no pages.
                chapter_pages[counting_index].assign(1, {});
                finish_counting();
            } else
                pages_failed = true;
            return;
        }
        const Chapter &source = in_place ? chapter : counting_chapter;
        const auto spec = layout(settings);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(15);
        do {
            if (stop && stop()) return;
            const auto start = pico_page::readable_offset(source, counting_offset);
            if (start >= source.text.size()) {
                if (source.last_part) {
                    finish_counting();
                    return;
                }
                ++counting_part;
                counting_offset = 0;
                if (!add_counting_part()) return;
                release_counting();
                return;
            }
            const auto planned = plan_page(source, start, spec, false, stop);
            if (planned.error == "Layout cancelled") return;
            if (!planned.error.empty() || planned.end <= planned.start) {
                pages_failed = true;
                release_counting();
                return;
            }
            if (!reserve_page_starts(chapter_pages[counting_index][counting_part], 1)) return;
            chapter_pages[counting_index][counting_part].push_back(static_cast<std::uint32_t>(planned.start));
            counting_offset = planned.end;
        } while (std::chrono::steady_clock::now() < deadline);
    }
    void draw_image_error(const graphics::LayoutSpec& spec) {
        outline(spec.left, spec.top + 12, spec.width, spec.height - 24, 2);
        center("图片无法显示", spec.left + 8, spec.top + spec.height / 2 - 36, spec.width - 16, 28);
        center("图片缺失、损坏或内存不足", spec.left + 8, spec.top + spec.height / 2 + 16, spec.width - 16, 22, 3);
    }
    bool draw_page_content(const graphics::Page& planned, const graphics::LayoutSpec& spec) {
        const StageTimer timer{"page_draw"};
        if (const auto* image = image_at(planned.start)) {
            if (image_prepared) {
                if (prepared_image && prepared_image_error.empty()) { canvas = *prepared_image; return true; }
                draw_image_error(spec);
                return true;
            }
            const auto cache_name = image_cache_key(document.metadata().book_id, image->resource, spec);
            if (body_cache_room()) {
                std::string cached;
                if (store.load_cache(cache_name, canvas.pixels().size() + 32, cached) == StoreStatus::Ok &&
                    cached.size() == canvas.pixels().size() + 32) {
                    const auto digest = cached.substr(cached.size() - 32);
                    cached.resize(cached.size() - 32);
                    if (md5_hex(cached) == digest) {
                        std::memcpy(canvas.data(), cached.data(), cached.size());
                        return true;
                    }
                }
            }
            std::vector<std::uint8_t> encoded;
            std::string detail;
            if (document.read_image(image->resource, encoded, detail) &&
                graphics::render_image(canvas, encoded, spec.left, spec.top, spec.width, spec.height, detail)) {
                std::vector<std::uint8_t>().swap(encoded);
                if (body_cache_room()) {
                    std::string pixels(reinterpret_cast<const char*>(canvas.data()), canvas.pixels().size());
                    pixels += md5_hex(pixels);
                    store.save_cache(cache_name, pixels);
                }
                return true;
            }
            draw_image_error(spec);
            return true;
        }
        return graphics::render_page(canvas, body, planned);
    }
    bool render_body(std::size_t start) {
        const StageTimer timer{"body_prepare"};
        const auto spec = layout(settings, link_return ? 144 : 64);
        if (body_canvas && start == page.start && same_layout(body_spec, spec)) {
            canvas = *body_canvas;
            links = body_links;
            offset = page.start;
            footer();
            return true;
        }
        const bool same_spec = same_layout(next_spec, spec);
        poll_raster();
        const bool prefetched = !next_chapter && next_page && same_spec && next_page->start == readable_offset(start);
        auto candidate = prefetched ? std::move(*next_page) : plan_page(start, spec);
        auto bitmap = prefetched ? std::move(next_bitmap) : nullptr;
        clear_prefetch();
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
        if (bitmap) { canvas = std::move(*bitmap); ++bitmap_hits; }
        else if (!draw_page_content(candidate, spec)) {
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
        if (body_cache_room()) { body_canvas = canvas; body_spec = spec; body_links = links; }
        else clear_body_cache();
        return true;
    }
    bool show_page(std::size_t start) {
        if (display.busy())
            return false;
        load_fonts();
        poll_raster();
        if (async_content && !image_prepared) {
            const auto spec = layout(settings, link_return ? 144 : 64);
            const bool retained = body_canvas && start == page.start && same_layout(body_spec, spec);
            const bool prefetched = !next_chapter && next_page && next_bitmap && next_page->start == readable_offset(start) && same_layout(next_spec, spec);
            if (!retained && !prefetched) if (const auto* image = image_at(readable_offset(start))) return begin_image(start, *image);
        }
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
        view = View::Reading;
        hits.clear();
        const auto token = session.request_page(
            anchor, (settings.refresh_preset == "clear" ||
                     body_updates >= static_cast<unsigned>(refresh_interval(settings)))
                        ? RefreshIntent::FullClean
                        : RefreshIntent::Content);
        if (!session.layout_ready(token))
            return false;
        return true;
    }
    /** Records only successfully presented body pages; at most 64 recent spans are retained. */
    void remember_page() {
        record_boundary(page_history, page.start, page.end);
    }
    std::optional<std::size_t> previous_page(std::size_t limit) const {
        for (const auto* entries : {&page_history, &computed_pages})
            for (auto i = entries->rbegin(); i != entries->rend(); ++i)
                if (i->chapter == chapter_index && i->part == chapter.part &&
                    i->return_bar == link_return.has_value() && i->end == limit && i->start < limit)
                    return i->start;
        if (link_return || chapter_index >= counted_live.size() || !counted_live[chapter_index] ||
            chapter.part >= chapter_pages[chapter_index].size()) return std::nullopt;
        const auto& starts = chapter_pages[chapter_index][chapter.part];
        const auto i = std::lower_bound(starts.begin(), starts.end(), limit);
        // A jump into the middle of a standard page is not a standard pagination boundary.
        if (i == starts.begin() || (limit != chapter.text.size() && (i == starts.end() || *i != limit)))
            return std::nullopt;
        return *std::prev(i);
    }
    /** Finds the preceding page, using matching boundaries before laying out from the part start. */
    bool paginate(std::size_t limit, std::size_t &previous) {
        if (const auto cached = previous_page(limit)) { previous = *cached; return true; }
        const auto spec = layout(settings, link_return ? 144 : 64);
        std::size_t start = 0;
        previous = 0;
        while (start < limit) {
            const auto planned = plan_page(chapter, start, spec, false);
            if (planned.start >= limit)
                break;
            if (planned.end <= start || !planned.error.empty())
                return false;
            record_boundary(computed_pages, planned.start, planned.end);
            previous = planned.start;
            start = planned.end;
        }
        return true;
    }
    void show_page_before(std::size_t limit) {
        std::size_t previous = 0;
        if (!paginate(limit, previous)) {
            error("上一页无法排版");
            return;
        }
        show_page(previous);
    }
    /**
     * Shows the preceding page. Cache misses over long text ranges are announced before
     * pagination because the first measurements may read glyph metrics from the card.
     */
    void turn_back_to(std::size_t limit) {
        if (const auto cached = previous_page(limit)) { show_page(*cached); return; }
        if (async_content) { begin_chapter(chapter_index, chapter.part, ContentJob::Target::Before, limit); return; }
        constexpr std::size_t announced_scalars = 10000;
        if (limit > announced_scalars)
            defer("正在排版…", [this, limit] { show_page_before(limit); });
        else
            show_page_before(limit);
    }
    // Reaching a later part parses the spine item from its beginning; an item this large is
    // announced first.
    static constexpr std::uint32_t announced_bytes = 150000;
    void change_part(std::size_t index, std::function<void()> work) {
        if (document.spine()[index].bytes > announced_bytes) defer("正在排版…", std::move(work));
        else work();
    }
    void turn(bool forward) {
        if (!has_book())
            return;
        turned = true;
        if (forward) {
            const auto next_offset = readable_offset(page.end);
            if (next_offset < chapter.text.size())
                show_page(next_offset);
            else if (!chapter.last_part) {
                const auto next = chapter.part + 1;
                if (async_content) { begin_chapter(chapter_index, next, ContentJob::Target::Start); return; }
                change_part(chapter_index, [this, next] {
                    if (load_chapter(chapter_index, next)) show_page(0);
                });
            } else if (chapter_index + 1 < document.spine().size()) {
                auto next = chapter_index + 1;
                while (next < document.spine().size() && !document.spine()[next].linear)
                    ++next;
                if (async_content && next < document.spine().size()) { begin_chapter(next, 0, ContentJob::Target::Start); return; }
                if (next < document.spine().size() && load_chapter(next))
                    show_page(0);
            } else {
                message = "已到全书末尾";
                draw_tools();
                overlay();
            }
        } else {
            if (offset == readable_offset(0)) {
                if (chapter.part > 0) {
                    const auto previous = chapter.part - 1;
                    if (async_content) { begin_chapter(chapter_index, previous, ContentJob::Target::End); return; }
                    change_part(chapter_index, [this, previous] {
                        if (load_chapter(chapter_index, previous)) show_page_before(chapter.text.size());
                    });
                    return;
                }
                if (chapter_index == 0)
                    return;
                auto previous_chapter = chapter_index - 1;
                while (previous_chapter > 0 && !document.spine()[previous_chapter].linear)
                    --previous_chapter;
                if (!document.spine()[previous_chapter].linear)
                    return;
                if (async_content) { begin_chapter(previous_chapter, kLastChapterPart, ContentJob::Target::End); return; }
                if (document.spine()[previous_chapter].bytes > announced_bytes) {
                    defer("正在排版…", [this, previous_chapter] {
                        if (load_chapter(previous_chapter, kLastChapterPart)) show_page_before(chapter.text.size());
                    });
                    return;
                }
                if (!load_chapter(previous_chapter, kLastChapterPart))
                    return;
                turn_back_to(chapter.text.size());
            } else {
                turn_back_to(offset);
            }
        }
    }
    void draw_tools() {
        tools_save_pending = session.needs_save();
        view = View::Tools;
        hits.clear();
        load_fonts();
        if (!render_body(offset)) return;
        for (const auto &line : page.lines)
            if ((line.top < 104 && line.bottom > 104) || (line.top < toolbar_top && line.bottom > toolbar_top))
                canvas.fill_rect(0, line.top, 684, line.bottom - line.top + 1, 15);
        reading_header();
        canvas.fill_rect(0, toolbar_top, 684, 136, 15);
        canvas.line(0, toolbar_top, 683, toolbar_top, 1, 3);
        const char *labels[] = {"目录", "排版", "同步", "刷新", "全刷"};
        const int ids[] = {Contents, Typography, Sync, Refresh, FullRefresh};
        for (int i = 0; i < 5; ++i) {
            const int x = i * 137;
            if (i) canvas.line(x, toolbar_top, x, 1215, 1, 2);
            tool_icon(ids[i], x + 68, 1126);
            const bool extra = ids[i] == Refresh;
            center(labels[i], x, extra ? 1148 : 1160, 137, 28);
            if (extra) center(settings.refresh_preset == "fast" ? "快速" : settings.refresh_preset == "clear" ? "清晰" : "均衡", x, 1186, 137, 22, 3);
            hits.push_back({x, toolbar_top, 137, 136, ids[i]});
        }
        if (!message.empty()) {
            canvas.fill_rect(24, 1012, 636, 56, 15);
            note(message, 1024);
        }
        hits.push_back({0, 104, 684, 908, Dismiss});
    }
    std::size_t current_toc() const {
        const auto &toc = document.toc();
        std::size_t current = toc.size();
        std::uint32_t best = 0;
        for (std::size_t i = 0; i < toc.size(); ++i) {
            if (toc[i].resource != chapter.resource) continue;
            std::uint32_t target = 0;
            if (!toc[i].fragment.empty() && !chapter.fragment_offset(toc[i].fragment, target)) continue;
            if (target <= offset && (current == toc.size() || target >= best)) { current = i; best = target; }
        }
        return current;
    }
    bool toc_parent(std::size_t index) const {
        const auto &toc = document.toc();
        return index + 1 < toc.size() && toc[index+1].depth > toc[index].depth;
    }
    void draw_contents() {
        view = View::Contents;
        hits.clear();
        canvas.clear();
        reading_header();
        tabs(104, Contents);
        const auto &toc = document.toc();
        const auto current = current_toc();
        toc_visible.clear();
        toc_starts = {0};
        auto row_height = [&](std::size_t index) {
            const auto &item = toc[index];
            const int width = (index == current ? 440 : 570) - std::min<int>(item.depth, 3) * 48 - (toc_parent(index) ? 88 : 0);
            return std::max(88, std::min(wrapped(item.label, 0, 0, width, 26, false), 80) + 24);
        };
        int used = 0;
        for (std::size_t i = 0; i < toc.size(); ++i) {
            const int height = row_height(i);
            if (used && used + height > 892) { toc_starts.push_back(toc_visible.size()); used = 0; }
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
        int y = 204;
        for (std::size_t row = begin; row < end; ++row) {
            const auto i = toc_visible[row];
            const auto &item = toc[i];
            const int h = row_height(i), indent = std::min<int>(item.depth, 3) * 48;
            const bool selected = i == current, parent = toc_parent(i);
            const int title_width = (selected ? 440 : 570) - indent - (parent ? 88 : 0);
            if (selected) outline(24, y + 3, 636, h - 6, 3);
            title_lines(item.label, 36 + indent, y + 20, title_width, item.depth ? 24 : 28, selected);
            if (selected) {
                const int x = parent ? 458 : 546;
                canvas.fill_rect(x, y + 28, 102, 36, 1);
                center("当前位置", x, y + 31, 102, 22, 15);
            }
            if (parent) {
                std::size_t count = 0;
                for (auto j = i + 1; j < toc.size() && toc[j].depth > item.depth; ++j) ++count;
                canvas.line(572, y, 572, y + h, 12, 2);
                const int cx = 590, cy = y + 44;
                if (toc_collapsed.count(i)) {
                    canvas.line(cx - 3, cy - 6, cx + 3, cy, 1, 3);
                    canvas.line(cx + 3, cy, cx - 3, cy + 6, 1, 3);
                } else {
                    canvas.line(cx - 6, cy - 3, cx, cy + 3, 1, 3);
                    canvas.line(cx, cy + 3, cx + 6, cy - 3, 1, 3);
                }
                center(std::to_string(count), 604, y + 24, 52, 26);
                hits.push_back({572, y, 88, h, TocToggleBase + static_cast<int>(i)});
            }
            canvas.line(24, y + h - 1, 660, y + h - 1, 12, 2);
            hits.push_back({24, y, parent ? 548 : 636, h, TocBase + static_cast<int>(i)});
            y += h;
        }
        if (toc.empty()) note("这本书未提供目录，可收起后继续阅读。", 256);
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
        const bool changed = typography_changed();
        outline(16, 12, changed ? 236 : 120, 40);
        text(changed ? "预览 · 尚未应用" : "当前排版", 28, 16, 22, 246);
        canvas.fill_rect(0, 656, 684, 560, 15);
        canvas.line(0, 656, 683, 656, 1, 3);
        tabs(656, Typography);
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
                return font_name(draft.font);
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
            center(value(ids[i]), x, 830, w, 26, 1, true);
            hits.push_back({x, 776, w, 96, ids[i]});
        }
        if (advanced)
            button(Advanced, "‹ 基本", 486, 776, 174, 96, false, 24);
        button(Minus, "−", 36, 904, 120, 88, false, 40);
        center(name(parameter), 168, 900, 348, 22);
        center(value(parameter), 168, 946, 348, 34, 1, true);
        button(Plus, "+", 528, 904, 120, 88, false, 40);
        canvas.line(0, 1012, 683, 1012, 12);
        text(changed ? "取消预览" : "作用域：此书", 36, 1034, 22, 190);
        if (changed) {
            canvas.line(36, 1064, 124, 1064, 1);
            hits.push_back({24, 1014, 190, 72, Back});
        }
        text("段距、缩进 >", 236, 1034, 22, 238);
        hits.push_back({220, 1014, 258, 72, Advanced});
        text("恢复默认", 514, 1034, 22, 146);
        hits.push_back({494, 1014, 166, 72, Defaults});
        button(Scope, "设为阅读默认", 36, 1088, 298, 96, false, 26);
        if (!font_fallback && changed)
            button(Apply, "应用排版", 350, 1088, 298, 96, true);
        else if (!font_fallback)
            disabled_button("已应用", 350, 1088, 298, 96);
        else {
            disabled_button("无法应用", 350, 1088, 298, 96);
            canvas.fill_rect(24, 586, 636, 58, 15);
            note("字体缺失或损坏，预览使用替代字体。", 596);
        }
    }
    static std::string font_name(const std::string &font) {
        return font.find("WenKai") != std::string::npos     ? "文楷"
               : font.find("NotoSans") != std::string::npos ? "思源黑体"
                                                            : basename(font);
    }
    static std::string decimal(double value) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.2f", value);
        return buf;
    }
    static std::string refresh_name(const ReaderSettings &values) {
        return values.refresh_preset == "clear" ? "清晰" : values.refresh_preset == "fast" ? "快速" : "均衡";
    }
    static std::string sleep_name(const ReaderSettings &values) {
        return values.sleep_screen == "reading" ? "保留正文" : values.sleep_screen == "simple" ? "简洁屏保" : "书籍封面";
    }
    static int refresh_interval(const ReaderSettings &values) {
        return values.refresh_preset == "fast" ? values.fast_full_refresh_pages : values.balanced_full_refresh_pages;
    }
    void choice(int id, const std::string &title, const std::string &description, int y, bool selected, bool pending_choice = false) {
        if (selected && pending_choice) dashed(24, y, 636, 116);
        else if (selected) outline(24, y, 636, 116, 3);
        for (int row = -18; row <= 18; ++row)
            for (int col = -18; col <= 18; ++col) {
                const int radius = row * row + col * col;
                if ((radius <= 324 && radius >= 225) || (selected && radius <= 100))
                    canvas.pixel(62 + col, y + 48 + row, 1);
            }
        text(title, 102, y + 16, 28, 430, 1, true);
        text(description, 102, y + 65, 22, 540, 3);
        if (selected) {
            canvas.fill_rect(540, y + 20, 102, 36, 1);
            center(pending_choice ? "待保存" : "当前", 540, y + 22, 102, 22, 15);
        }
        hits.push_back({24, y, 636, 116, id});
    }
    bool panel_background(int top) {
        hits.clear();
        if (has_book()) {
            load_fonts();
            if (!render_body(offset)) return false;
            for (const auto &line : page.lines)
                if ((line.top < 104 && line.bottom > 104) || (line.top < top && line.bottom > top))
                    canvas.fill_rect(0, line.top, 684, line.bottom - line.top + 1, 15);
            reading_header();
        } else {
            canvas.clear();
            header("显示与刷新", Back, "设置");
        }
        canvas.fill_rect(0, top, 684, 1216 - top, 15);
        if (refresh_from_settings) {
            hits.clear();
            header("显示与刷新", Back, "设置");
        }
        return true;
    }
    void draw_refresh() {
        view = View::Refresh;
        if (!panel_background(502)) return;
        if (refresh_from_settings) {
            text("阅读默认 · 本书覆盖项保持不变", 36, 536, 24, 612, 3);
            canvas.line(0, 502, 683, 502, 1, 3);
        } else tabs(502, Refresh);
        const auto &values = edit_snapshot.config.settings;
        choice(RefreshClear, "清晰", "每页执行全刷，减少残影", 616, values.refresh_preset == "clear");
        choice(RefreshBalanced, "均衡", "灰阶更新，定期全刷", 752, values.refresh_preset == "balanced");
        choice(RefreshFast, "快速", "优先响应速度，可能更易留痕", 884, values.refresh_preset == "fast");
        button(FullRefresh, "立即全刷", 36, 1020, 612, 96);
        if (values.refresh_preset == "clear") note("自动全刷间隔：每页", 1150);
        else {
            text("自动全刷间隔", 36, 1150, 24, 360);
            text("每 " + std::to_string(refresh_interval(values)) + " 页 ›", 490, 1150, 24, 170);
            hits.push_back({24, 1128, 636, 88, RefreshInterval});
        }
    }
    void draw_refresh_confirm() {
        draw_refresh();
        if (view == View::Error) return;
        view = View::RefreshConfirm;
        hits.clear();
        canvas.fill_rect(0, 752, 684, 464, 15);
        canvas.line(0, 752, 683, 752, 1, 3);
        text("改用快速刷新？", 36, 788, 32, 612, 1, true);
        wrapped("快速模式可能减少灰阶或更易留下残影。可随时按 K2 全刷当前画面。", 36, 858, 612, 26);
        note("确认前保留当前刷新模式。", 996);
        button(Back, "保持" + refresh_name(edit_snapshot.config.settings), 24, 1080, 306, 96);
        button(RefreshAccept, "改用快速", 354, 1080, 306, 96, true);
    }
    void draw_interval() {
        view = View::RefreshInterval;
        hits.clear(); canvas.clear(); header("自动全刷间隔");
        text(refresh_name(draft) + "模式", 36, 170, 32, 612, 1, true);
        wrapped("间隔越短，清理残影越频繁。此设置不影响 K2 手动全刷。", 36, 246, 612, 26);
        button(IntervalLess, "−", 36, 440, 120, 96);
        center("每 " + std::to_string(refresh_interval(draft)) + " 页", 168, 460, 348, 34);
        button(IntervalMore, "+", 528, 440, 120, 96);
        note("可设为 1 至 30 页。", 582);
        button(Back, "取消", 24, 1080, 306, 96);
        button(IntervalSave, "保存间隔", 354, 1080, 306, 96, true);
    }
    void settings_row(int id, const std::string &title, const std::string &value, int y) {
        text(title, 36, y + 20, 28, 554, 1, true);
        text(value, 36, y + 70, 22, 554, 3);
        text("›", 620, y + 42, 28, 40);
        canvas.line(24, y + 131, 660, y + 131, 12, 2);
        hits.push_back({24, y, 636, 132, id});
    }
    void draw_settings() {
        view = View::Settings;
        hits.clear(); canvas.clear(); header("设置", Back, "书库");
        const auto cfg = store.load_settings();
        const auto &values = cfg.settings;
        sync::Config account;
        std::string detail;
        const bool configured = sync::load_config(root, account, detail);
        std::uint64_t card_total = 0, card_free = 0;
        const bool measured = store.card_space(card_total, card_free);
        provision::WifiCredentials wifi;
        settings_row(Import, "网络与传输", std::string("Wi-Fi：") +
                     (provision::load_wifi(root, wifi, detail) ? wifi.ssid : "未配置") + " · USB 文件传输", 120);
        settings_row(SyncSettings, "KOReader 同步", configured ? server_name(account) + " · " + account.username : "未配置", 252);
        settings_row(Refresh, "显示与刷新", refresh_name(values) + " · " +
                     (values.refresh_preset == "clear" ? "每页全刷" : "每 " + std::to_string(refresh_interval(values)) + " 页全刷"), 384);
        settings_row(InputSettings, "输入与阅读", std::string(values.library_view == "grid" ? "卡片" : "列表") + "视图 · " + typography_summary(values), 516);
        settings_row(PowerSettings, "电源与休眠", sleep_name(values) + " · " +
                     (values.auto_sleep_minutes ? std::to_string(values.auto_sleep_minutes) + " 分钟" : "不自动休眠"), 648);
        settings_row(StorageSettings, "存储与设备", (measured ? "可用 " + file_size(card_free) : "可用空间未知") + " · 固件 " + firmware_version(), 780);
        if (!cfg.diagnostics.empty()) note("设置文件无效，请检查后重新打开。", 950);
        else if (!message.empty()) note(message, 950);
        if (session.needs_save()) button(SaveRetry, "重试保存进度", 24, 1056, 636, 96);
    }
    void draw_power() {
        view = View::PowerSettings;
        hits.clear(); canvas.clear(); header("电源与休眠", Back, "设置");
        text("休眠画面", 36, 144, 24, 612, 3);
        choice(SleepCover, "书籍封面", "无可用封面时显示书名与作者", 200, draft.sleep_screen == "cover", draft.sleep_screen != edit_snapshot.config.settings.sleep_screen);
        choice(SleepReading, "保留正文", "保留当前页，底部显示休眠状态", 332, draft.sleep_screen == "reading", draft.sleep_screen != edit_snapshot.config.settings.sleep_screen);
        choice(SleepSimple, "简洁屏保", "仅显示书名与阅读位置", 464, draft.sleep_screen == "simple", draft.sleep_screen != edit_snapshot.config.settings.sleep_screen);
        text("自动休眠", 36, 636, 28, 612, 1, true);
        button(SleepLess, "−", 36, 706, 120, 96);
        center(draft.auto_sleep_minutes ? std::to_string(draft.auto_sleep_minutes) + " 分钟" : "关闭", 168, 728, 348, 34);
        button(SleepMore, "+", 528, 706, 120, 96);
        note("按电源键唤醒；保存失败时不进入休眠。", 856);
        note("选择后点击保存生效。", 930);
        button(Back, "取消", 24, 1080, 306, 96);
        button(PowerSave, "保存设置", 354, 1080, 306, 96, true);
    }
    static std::string server_name(const sync::Config &account) {
        const auto scheme = account.endpoint.find("://");
        return scheme == std::string::npos ? account.endpoint : account.endpoint.substr(scheme + 3);
    }
    static std::string typography_summary(const ReaderSettings &values) {
        return font_name(values.font) + " · " + std::to_string(values.font_size_px) + " px · " + decimal(values.line_height);
    }
    static std::string firmware_version() {
#ifdef ESP_PLATFORM
        return esp_app_get_description()->version;
#else
        return "桌面预览";
#endif
    }
    /**
     * Sums regular files below a card-relative directory; epub_only keeps EPUB names only.
     * The walk blocks the application task, so it stops after 2000 entries. nullopt when a
     * directory cannot be read or that limit is exceeded.
     */
    std::optional<std::uint64_t> tree_bytes(const std::string &relative, bool epub_only) {
        unsigned remaining = 2000;
        std::uint64_t total = 0;
        std::vector<std::string> pending{relative};
        while (!pending.empty()) {
            const auto directory = std::move(pending.back());
            pending.pop_back();
            DIR *dir = opendir((root + "/" + directory).c_str());
            if (!dir) return std::nullopt;
            while (const auto *item = readdir(dir)) {
                const std::string name = item->d_name;
                if (name == "." || name == "..") continue;
                struct stat st{};
#ifdef ESP_PLATFORM
                const int result = stat((root + "/" + directory + "/" + name).c_str(), &st);
#else
                const int result = lstat((root + "/" + directory + "/" + name).c_str(), &st);
#endif
                if (result != 0 || !remaining--) {
                    closedir(dir);
                    return std::nullopt;
                }
                if (S_ISDIR(st.st_mode)) pending.push_back(directory + "/" + name);
                else if (S_ISREG(st.st_mode) && (!epub_only || epub_name(name))) total += static_cast<std::uint64_t>(st.st_size);
            }
            closedir(dir);
        }
        return total;
    }
    void draw_storage() {
        view = View::StorageSettings;
        hits.clear(); canvas.clear(); header("存储与设备", Back, "设置");
        std::optional<std::uint64_t> books = 0;
        for (const auto &path : store.load_settings().settings.library_roots) {
            struct stat st{};
            if (!safe_existing(root, path) || stat((root + "/" + path).c_str(), &st) != 0) continue;
            const auto bytes = tree_bytes(path, true);
            books = books && bytes ? std::optional<std::uint64_t>(*books + *bytes) : std::nullopt;
        }
        struct stat cache_directory{};
        const auto cache = stat((root + "/PicoPage/cache").c_str(), &cache_directory) == 0
                               ? tree_bytes("PicoPage/cache", false) : std::optional<std::uint64_t>(0);
        std::uint64_t card_total = 0, card_free = 0;
        const bool measured = store.card_space(card_total, card_free);
        const auto size = [](const std::optional<std::uint64_t> &bytes) { return bytes ? file_size(*bytes) : std::string("未知"); };
        text("存储卡", 36, 128, 22, 612, 3);
        value_row(0, "书籍原文件", "", size(books), 164, 70);
        value_row(0, "缓存（可重建）", "", size(cache), 234, 70);
        value_row(0, "可用空间", "", measured ? file_size(card_free) : "未知", 304, 70, true);
        text("设备", 36, 402, 22, 612, 3);
        value_row(0, "固件版本", "", firmware_version(), 438, 70);
#ifdef ESP_PLATFORM
        value_row(0, "硬件", "", "Read Pico", 508, 70);
#else
        value_row(0, "硬件", "", "桌面预览", 508, 70);
#endif
        text("文件位置", 36, 606, 22, 612, 3);
        value_row(0, "设置", "", "PicoPage/settings.toml", 642, 70);
        value_row(0, "阅读进度", "", "原书旁的 .picopage 目录", 712, 70);
        if (!message.empty()) note(message, 830);
        canvas.line(0, 1056, 683, 1056, 1, 3);
        text("清理全部缓存", 36, 1086, 26, 540, 1, true);
        text("不删除书和阅读进度，下次打开会稍慢", 36, 1130, 20, 540, 3);
        text("›", 620, 1098, 28, 40);
        hits.push_back({0, 1058, 684, 158, ClearCache});
    }
    void draw_input_settings() {
        view = View::InputSettings;
        hits.clear(); canvas.clear(); header("输入与阅读", Back, "设置");
        const auto values = store.load_settings().settings;
        value_row(LibraryView, "书库默认视图", "", values.library_view == "grid" ? "卡片" : "列表", 104);
        value_row(has_book() ? Typography : 0, "阅读默认排版", typography_summary(values), "", 208);
        value_row(FullRefresh, "立即全刷", "K2 在所有页面全刷当前画面，不改变位置", "", 312);
        text("正文翻页", 36, 452, 22, 612, 3);
        note("K1 上一页，K3 下一页。", 496);
        note("触摸正文左右区域翻页，中部打开工具。", 548);
        note("关闭和取消使用屏幕控件。", 600);
        if (!has_book()) note("打开一本书后，可在排版页调整阅读默认排版。", 652);
        if (!message.empty()) {
            canvas.line(0, 1012, 683, 1012, 1, 3);
            wrapped(message, 36, 1044, 612, 22);
        }
    }
    void draw_sync_settings() {
        view = View::SyncSettings;
        hits.clear(); canvas.clear(); header("KOReader 同步", Back, "设置");
        sync::Config account;
        std::string detail;
        const bool configured = sync::load_config(root, account, detail);
        value_row(0, "服务地址", "", configured ? server_name(account) : "未配置", 104, 70);
        value_row(0, "账户", "", configured ? account.username : "未配置", 174, 70);
        value_row(0, "凭据", "", configured ? "已隐藏" : "未配置", 244, 70);
        wrapped(configured ? "可用手机配置修改地址或账户，也可在电脑上编辑 PicoPage/sync.toml。"
                           : "PicoPage/sync.toml 不存在或内容无效。可用手机配置填写服务地址和账户。",
                36, 340, 612, 22);
        text("同步策略", 36, 452, 22, 612, 3);
        value_row(0, "检查与上传", "在正文工具的“同步”中手动执行", "手动", 488);
        value_row(0, "书籍匹配方式", "按原书内容识别，与文件名无关", "按内容", 592);
        button(PortalStart, "用手机配置", 24, 760, 636, 96);
        if (has_book() && configured) button(Sync, "检查当前书籍的同步", 24, 1056, 636, 96);
        else note(configured ? "打开一本书后，可在工具中检查同步。" : "配置完成前不会联网。", 1090);
    }
    static std::string file_size(std::uint64_t bytes) {
        if (bytes < 1024) return std::to_string(bytes) + " B";
        if (bytes < 1024 * 1024) return std::to_string((bytes + 1023) / 1024) + " KiB";
        char value[40];
        if (bytes < 1024ULL * 1024 * 1024) std::snprintf(value, sizeof(value), "%.1f MiB", bytes / 1048576.0);
        else std::snprintf(value, sizeof(value), "%.1f GiB", bytes / 1073741824.0);
        return value;
    }
    void inspect_book(const std::string &path, bool keep_message = false) {
        if (!keep_message) message.clear();
        detail_path = path; detail_metadata = {}; detail_progress = {};
        detail_position.clear(); detail_error.clear(); detail_size.reset();
        struct stat st{};
        if (!safe_existing(root, path) || stat((root + "/" + path).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            detail_error = "原文件不可用，请检查 TF 卡。";
        } else {
            detail_size = st.st_size;
            EpubDocument inspected;
            if (inspected.open(root + "/" + path, detail_error)) {
                const auto *source = &inspected;
                detail_metadata = source->metadata();
                detail_progress = store.load_progress(path, detail_metadata.book_id, content_revision);
                if (detail_progress.record) {
                    const auto &anchor = detail_progress.record->anchor;
                    for (const auto &item : source->toc())
                        if (item.resource == anchor.resource) { detail_position = item.label; break; }
                    if (detail_position.empty()) detail_position = "已保存阅读位置";
                }
            }
        }
        draw_details();
    }
    void draw_details() {
        view = View::BookDetails;
        hits.clear(); canvas.clear(); header("书籍详情", Back, "书库");
        const auto title = detail_metadata.title.empty() ? basename(detail_path) : detail_metadata.title;
        struct stat info{};
        std::vector<std::uint8_t> cover;
        if (detail_error.empty() && stat((root + "/" + detail_path).c_str(), &info) == 0)
            cover = cover_pixels(detail_path, info, 120, 160, false);
        const bool covered = !cover.empty();
        if (covered) blit(cover, 36, 140, 120, 160);
        const int title_x = covered ? 180 : 36, title_width = covered ? 468 : 612;
        int size = 32;
        while (size > 22 && wrapped(title, 0, 0, title_width, size, false) > 160) size -= 2;
        title_lines(title, title_x, 140, title_width, size, true, 4);
        text(detail_metadata.author.empty() ? "作者未知" : detail_metadata.author, 36, 324, 24, 612, 3);
        canvas.line(24, 378, 660, 378, 1, 2);
        text("进度", 36, 408, 24, 130, 3);
        text(detail_progress.record ? detail_position : (detail_progress.status == StoreStatus::Missing || detail_progress.status == StoreStatus::Cleared) ? "尚无本地阅读记录" : "阅读记录无法验证", 186, 408, 24, 462);
        text("同步", 36, 478, 24, 130, 3);
        text(detail_path == book_path && detail_metadata.book_id == document.metadata().book_id ? sync_badge() : "打开书籍后检查", 186, 478, 24, 462);
        text("原书占用", 36, 548, 24, 150, 3);
        text(detail_size ? file_size(*detail_size) : "未知", 210, 548, 24, 438);
        if (detail_error.empty()) button(DetailOpen, detail_progress.record ? "继续阅读" : "打开书籍", 36, 630, 612, 96, true);
        else wrapped("无法打开：" + detail_error, 36, 620, 612, 24);
        canvas.line(0, 764, 683, 764, 1, 3);
        button(DetailCache, "清理本书缓存", 36, 788, 612, 76);
        if (detail_error.empty()) button(DetailReset, "清除本地阅读进度", 36, 880, 612, 76);
        button(DetailDelete, "删除原书文件", 36, 972, 612, 76);
        if (!message.empty()) note(message, 1062);
        button(Back, "返回书库", 24, 1120, 636, 80);
    }
    void confirm_management(bool remove) {
        if (!persist()) { error(message); return; }
        EpubDocument inspected;
        std::string detail;
        if (!remove && (!inspected.open(root + "/" + detail_path, detail) ||
                        inspected.metadata().book_id != detail_metadata.book_id)) {
            error("原书已变化，请重新打开详情。"); return;
        }
        if (store.book_snapshot(detail_path, management_snapshot) != StoreStatus::Ok) {
            error("无法读取文件状态，请检查 TF 卡后重新打开详情。"); return;
        }
        delete_original = remove;
        view = View::BookManagement;
        hits.clear(); canvas.clear(); header("书籍管理", 0);
        text(remove ? "删除这本书？" : "清除本地进度？", 36, 184, 34, 612, 1, true);
        title_lines(detail_metadata.title.empty() ? basename(detail_path) : detail_metadata.title, 36, 280, 612, 28, true, 3);
        title_lines(detail_path, 36, 448, 612, 22, false, 3);
        wrapped(remove ? "删除 TF 卡中的原书文件。阅读记录与排版设置保留，重新放回相同原书后可恢复。" :
                "清除本书的本地阅读位置及其备份，下次打开从头开始。原书和排版设置保留。", 36, 624, 612, 28);
        note("不会修改服务器上的阅读记录。", 876);
        note("K2 仅全刷；请使用下方按钮选择。", 942);
        button(Back, "取消", 24, 1080, 306, 96, true);
        button(ManageConfirm, remove ? "删除原书" : "清除进度", 354, 1080, 306, 96);
    }
    void manage_book() {
        if (detail_path == book_path) {
            cancel_sync();
            session.open("", "", writer);
            document.close(); book_path.clear(); release(chapter); page = {};
            links.clear(); clear_page_history(); link_return.reset(); confirmed_link_return.reset();
            last_recorded.clear();
        }
        abandon_content(); foreground_job.reset(); stop_covers(); stop_raster();
        stop_counting_job();
        identities.clear();
        font_identity.clear(); effective_font.clear();
        const auto result = store.manage_book(management_snapshot, delete_original, detail_metadata.book_id, content_revision);
        if (result != StoreStatus::Ok) {
            error(result == StoreStatus::ExternalChange ? "文件已被外部修改，操作未执行。请重新打开书籍详情。" :
                  "操作未完成，请检查 TF 卡并重新打开详情核实文件状态。");
            return;
        }
        if (acknowledged_record && acknowledged_record->book_id == detail_metadata.book_id) acknowledged_record.reset();
        if (uncertain_record && uncertain_record->book_id == detail_metadata.book_id) uncertain_record.reset();
        covers.erase(std::remove_if(covers.begin(), covers.end(), [&](const auto& cover) { return cover.path == detail_path; }), covers.end());
        if (delete_original) {
            if (last_book == detail_path) last_book.clear();
            message = "原书已删除，阅读记录保留";
            scan_library(); draw_library();
        } else {
            message = "本地进度已清除，远端记录未修改";
            inspect_book(detail_path, true);
        }
        overlay();
    }
    void cover_title(const std::string &title, int x, int top, int width, int height, int initial_size) {
        const auto content = decode_utf8(title);
        graphics::LayoutSpec spec;
        spec.left = x; spec.top = top; spec.width = width; spec.height = height;
        spec.indent_chars = 0;
        graphics::Page title_page;
        for (int size = initial_size; size >= 28; size -= 2) {
            body.size(size);
            spec.font_px = size;
            spec.line_height = std::max(size * 5 / 4, body.metrics().ascender + body.metrics().descender);
            title_page = graphics::layout_page(body, content, 0, spec);
            if (title_page.end == content.size()) break;
        }
        graphics::render_page(canvas, body, title_page);
    }
    void draw_sleep(const std::string &sync_state) {
        hits.clear();
        load_fonts();
        if (has_book() && !restore_confirmed_position()) return;
        const auto position = has_book() ? book_position(offset, offset) : std::string("位置已保存");
        if (settings.sleep_screen == "simple") {
            canvas.clear();
            text("已休眠", 72, 440, 40, 540, 1, true);
            text("按电源键继续阅读", 72, 512, 26, 540);
            canvas.line(72, 594, 612, 594, 1, 2);
            cover_title(has_book() ? document.metadata().title : "Pico Page", 72, 626, 540, 140, 32);
            text(has_book() ? chapter_label() + " · " + position : "尚未打开书籍", 72, 806, 22, 540, 3);
            text(has_book() ? "此处已保存 · " + sync_state : "", 72, 854, 22, 540, 3);
            view = View::Sleeping;
            return;
        }
        const bool reading = has_book() && settings.sleep_screen == "reading";
        if (reading) {
            load_fonts();
            if (!render_body(offset)) return;
            for (const auto &line : page.lines)
                if (line.top < 1088 && line.bottom > 1088)
                    canvas.fill_rect(0, line.top, 684, line.bottom - line.top + 1, 15);
        } else {
            canvas.clear();
            bool covered = false;
            if (has_book() && settings.sleep_screen == "cover") {
                struct stat info{};
                std::vector<std::uint8_t> cover;
                if (safe_existing(root, book_path) && stat((root + "/" + book_path).c_str(), &info) == 0)
                    cover = cover_pixels(book_path, info, 684, 1040, true);
                covered = !cover.empty();
                if (covered) blit(cover, 0, 0, 684, 1040);
            }
            if (!covered) {
                if (settings.sleep_screen == "cover") outline(56, 56, 572, 928, 3);
                cover_title(has_book() ? document.metadata().title : "Pico Page", 112, 286, 460, 384, 76);
                canvas.fill_rect(112, 724, 96, 4, 1);
                text(has_book() ? document.metadata().author : "个人阅读", 112, 780, 28, 460);
            }
        }
        const int top = reading ? 1088 : 1040;
        canvas.fill_rect(0, top, 684, 1216 - top, 1);
        text("已休眠", 36, top + 24, 30, 290, 15, true);
        text("按电源键继续阅读", 36, top + 72, 22, 290, 15);
        text(has_book() ? "此处已保存" : "尚未打开书籍", 364, top + 24, 22, 284, 15);
        text(has_book() ? sync_state : "", 364, top + 60, 22, 284, 15);
        if (!reading && has_book()) text(position, 364, top + 106, 22, 284, 15);
        view = View::Sleeping;
    }
    void draw_import() {
        view = View::Import;
        hits.clear();
        canvas.clear();
        header("网络与传输", Back, import_from_settings ? "设置" : "书库");
        text("USB 文件传输", 36, 128, 22, 612, 3);
        note("开启后，电脑可以直接管理 TF 卡。", 172);
        note("传输期间暂停阅读和同步。", 220);
        note("请在电脑安全弹出后退出。", 268);
        note("退出将重启，恢复阅读与 USB 烧录。", 316);
        button(UsbStart, "开启 USB 文件传输", 24, 384, 636, 96);
        canvas.line(36, 520, 647, 520, 12, 2);
        text("Wi-Fi 与 KOReader 同步", 36, 548, 22, 612, 3);
        provision::WifiCredentials wifi;
        std::string detail;
        note(provision::load_wifi(root, wifi, detail) ? "Wi-Fi：" + wifi.ssid : "Wi-Fi 未配置", 592);
        note("用手机填写，设备验证后保存。", 640);
        button(PortalStart, "开启手机配置", 24, 708, 636, 96);
        if (!message.empty()) note(message, 860);
        button(Dismiss, "返回书库", 24, 1056, 636, 96);
    }
    void draw_portal() {
        view = View::Portal;
        hits.clear();
        canvas.clear();
        header("手机配置", 0);
        if (portal.access_ssid.empty()) {
            note(portal_closing ? "正在结束配置…" : "正在开启设备热点…", 200);
        } else {
            text("1  用手机连接设备热点", 36, 128, 26, 612, 1, true);
            graphics::draw_qr(canvas, provision::wifi_qr({portal.access_ssid, portal.access_password}), 36, 176, 264);
            text("热点名称", 324, 196, 22, 324, 3);
            text(portal.access_ssid, 324, 230, 28, 324);
            text("密码", 324, 300, 22, 324, 3);
            text(portal.access_password, 324, 334, 28, 324);
            text("扫码可直接连接", 324, 400, 22, 324, 3);
            text("2  连接后扫码打开配置页", 36, 472, 26, 612, 1, true);
            graphics::draw_qr(canvas, portal.url, 36, 520, 264);
            text("或在浏览器输入", 324, 540, 22, 324, 3);
            const auto scheme = portal.url.find("://");
            text(scheme == std::string::npos ? portal.url : portal.url.substr(scheme + 3), 324, 574, 28, 324);
            text("只支持 2.4 GHz 网络", 324, 660, 22, 324, 3);
            canvas.line(0, 816, 683, 816, 1, 3);
            std::string title, body = portal.detail;
            switch (portal.phase) {
            case PortalPhase::JoiningWifi:
                title = "正在连接“" + portal.detail + "”…";
                body = "验证期间，手机可能暂时断开设备热点。";
                break;
            case PortalPhase::CheckingAccount:
                title = "正在验证同步账户…";
                body.clear();
                break;
            case PortalPhase::Saved:
                title = "已保存";
                body += "可以结束配置，或在手机上继续修改。";
                break;
            case PortalPhase::Failed:
                title = "未保存";
                body += "请在手机上重新填写。";
                break;
            default:
                title = "等待手机提交";
                body = "在配置页填写后，设备先验证，成功后才保存。";
            }
            if (portal_closing) { title = "正在结束配置…"; body.clear(); }
            text(title, 36, 844, 28, 612, 1, true);
            wrapped(body, 36, 904, 612, 24);
        }
        if (!portal_closing) button(PortalStop, "结束配置", 24, 1080, 636, 96);
    }
    void draw_usb() {
        view = View::UsbTransfer;
        hits.clear();
        canvas.clear();
        // TF ownership may already belong to the host; this page uses no card resources.
        center("USB 文件传输", 0, 100, 684, 36);
        if (usb_confirm) {
            note("请先在电脑安全弹出设备。", 340);
            note("确认后将退出传输并重启。", 420);
            button(UsbCancel, "取消", 24, 1056, 306, 96);
            button(UsbExitConfirm, "退出并重启", 354, 1056, 306, 96);
        } else {
            note(usb_active ? "USB 文件传输中" : "USB 文件传输未开启", 300);
            note("电脑使用 TF 卡时，设备暂停阅读。", 390);
            note("请先安全弹出，再退出并重启。", 470);
            if (!message.empty()) note(message, 650);
            button(UsbExit, "退出并重启", 24, 1056, 636, 96);
        }
    }
    static bool same_account(const sync::Config &a, const sync::Config &b) {
        return a.endpoint == b.endpoint && a.username == b.username && a.userkey == b.userkey &&
               a.device_id == b.device_id && a.dom_version == b.dom_version;
    }
    void cancel_sync() {
        if (active_network && uploading && upload_record) {
            uncertain_record = upload_record;
            uncertain_config = sync_config;
        }
        if (network_cancel)
            network_cancel->store(true);
        active_network = 0;
        network_request.reset();
        remote_anchor.reset();
        upload_record.reset();
        sync_checked = false;
        uploading = false;
    }
    std::string anchor_label(const Anchor& anchor) const {
        for (const auto& item : document.toc())
            if (item.resource == anchor.resource) return item.label;
        return basename(anchor.resource);
    }
    void draw_sync() {
        view = View::Sync;
        if (!has_book()) {
            hits.clear(); canvas.clear(); header("阅读同步", Back, "书库");
            note("请先打开一本 EPUB。", 246); return;
        }
        refresh_from_settings = false;
        if (!panel_background(508)) return;
        // All controls above the panel are disabled while choosing a remote candidate.
        hits.clear();
        if (remote_anchor && !active_network) {
            canvas.line(0, 508, 683, 508, 1, 3);
            text("发现不同的阅读位置", 36, 550, 32, 612, 1, true);
            note("请选择继续位置；不会自动覆盖远端。", 612);
            outline(36, 674, 297, 210, 3); outline(351, 674, 297, 210, 2);
            text("本机", 56, 694, 24, 258, 3);
            title_lines(session.saved_record() ? anchor_label(session.saved_record()->anchor) : "尚未保存", 56, 742, 256, 28, true, 2);
            text(session.needs_save() ? "此处尚未保存" : "本地已保存", 56, 838, 22, 256);
            text("远端记录", 371, 694, 24, 256, 3);
            title_lines(anchor_label(*remote_anchor), 371, 742, 256, 28, true, 2);
            text("源位置已验证", 371, 838, 22, 256);
            button(Back, "继续此处", 36, 900, 297, 96);
            button(SyncJump, "使用远端位置", 351, 900, 297, 96, true);
            note("继续此处只关闭提示，不上传。", 1030);
            button(SyncUpload, "上传此处，替换远端位置", 36, 1092, 612, 88);
            return;
        }
        tabs(508, Sync);
        std::string detail;
        sync::Config configured;
        const bool ready = sync::load_config(root, configured, detail);
        if (!ready && !active_network) { server_state = "未配置"; remote_state = "未检查"; }
        for (int x : {228, 456}) canvas.line(x, 610, x, 795, 1, 2);
        canvas.line(0, 795, 683, 795, 1, 2);
        text("本机", 36, 634, 24, 185, 3);
        text(session.needs_save() ? "未保存" : session.saved_record() ? "已保存" : "无记录", 36, 682, 30, 185, 1, true);
        text("当前正文位置", 36, 740, 22, 185);
        text("服务器", 248, 634, 24, 185, 3);
        text(server_state, 248, 682, 28, 185, 1, true);
        text(active_network ? (uploading ? "提交已存版本" : "读取远端记录") : "本次请求状态", 248, 740, 22, 190);
        text("远端记录", 476, 634, 24, 185, 3);
        text(remote_state, 476, 682, 28, 185, 1, true);
        text("非设备实时状态", 476, 740, 20, 188, 3);
        if (!ready && !active_network) {
            wrapped("同步未配置或配置无效。请在电脑上编辑配置文件后重试。", 36, 822, 612, 26);
            note("服务：PicoPage/sync.toml", 938);
            note("网络：PicoPage/network.toml", 992);
            button(SyncCheck, "重新检查配置", 36, 1056, 612, 96);
            return;
        }
        title_lines(sync_status.empty() ? (sync_badge() == "待核实" ? "上次上传结果未确认，请重新检查远端。" : "先读取远端位置，再决定是否上传。") : sync_status,
                    36, 820, 612, 26, false, 2);
        if (active_network) {
            dashed(36, 916, 612, 96);
            center(uploading ? "上传中" : "检查中", 36, 946, 612, 28);
            button(Back, "取消同步", 36, 1032, 612, 96);
            note(uploading ? "取消后，已发出的上传仍可能生效。" : "取消后保留本机位置。", 1160);
        } else {
            button(SyncCheck, "检查远端位置", 36, 916, 612, 96, !sync_checked);
            if (sync_checked) button(SyncUpload, "上传本机位置", 36, 1032, 612, 96);
            else button(Back, "继续阅读", 36, 1032, 612, 96);
            note(acknowledged_record && session.saved_record() && *acknowledged_record == *session.saved_record() && same_account(configured, acknowledged_config)
                 ? "此版本已收到服务器上传确认。" : "仅上传已成功显示并保存的位置。", 1160);
        }
    }
    void check_sync() {
        cancel_sync();
        server_state = "检查中"; remote_state = "检查中";
        std::string detail;
        refresh_badge_config();
        if (!sync::load_config(root, sync_config, detail)) {
            server_state = "未配置"; remote_state = "未检查";
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
        if (!panel_background(752)) return;
        view = View::Confirmation;
        hits.clear();
        canvas.line(0, 752, 683, 752, 1, 3);
        text("用本机位置更新远端？", 36, 786, 32, 612, 1, true);
        wrapped("仅上传当前已保存的阅读位置。其他设备下次同步会收到此位置。", 36, 864, 612, 26);
        note("确认后将替换服务器上的记录。", 984);
        button(Back, "取消", 24, 1080, 306, 96, true);
        button(SyncConfirm, "确认上传", 354, 1080, 306, 96);
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
            server_state = "配置变化"; remote_state = "未检查";
            sync_status = "同步配置已更改，请重新检查远端位置";
            draw_sync();
            overlay();
            return;
        }
        const auto record = *session.saved_record();
        std::string xpointer;
        Chapter source;
        std::size_t index;
        const auto holds = [&](const Chapter &part) {
            std::uint32_t ignored;
            return part.offset_for(record.anchor, ignored);
        };
        if (!document.chapter_index_for(record.anchor.resource, index) ||
            !find_content(store, document, index, source, detail, ChapterLoad::Mapping, holds) ||
            !sync::to_xpointer(document, source, record.anchor, sync_config.dom_version, xpointer,
                               detail)) {
            sync_status = memory_error(detail) ? "设备内存不足，暂时无法计算同步位置。" : "此位置暂不支持精确同步：" + detail;
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
        server_state = "上传中";
        sync_status = "正在上传已保存的位置";
        draw_sync();
        overlay();
    }
    void network_complete(std::uint64_t id, int status, const std::string &bytes,
                          const std::string &failure) {
        if (id != active_network || !active_network)
            return;
        if (uploading && upload_record) { uncertain_record = upload_record; uncertain_config = sync_config; }
        active_network = 0;
        if (view != View::Sync) {
            cancel_sync();
            return;
        }
        sync::Config current;
        std::string detail;
        if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
            cancel_sync();
            server_state = "配置变化"; remote_state = "未检查";
            sync_status = "同步配置已更改，本次结果已丢弃";
        } else if (!failure.empty()) {
            if (uploading && upload_record) { uncertain_record = upload_record; uncertain_config = sync_config; }
            sync_checked = false;
            remote_anchor.reset();
            server_state = "请求失败"; remote_state = "未确认";
            sync_status = uploading ? "上传结果未确认，请检查远端位置。" : "连接失败，请检查网络后重试。";
        } else if (uploading) {
            remote_anchor.reset();
            if (sync::upload_confirmed(status, detail) && upload_record && session.saved_record() &&
                *session.saved_record() == *upload_record) {
                acknowledged_record = upload_record; acknowledged_config = sync_config; uncertain_record.reset();
                server_state = "已确认"; remote_state = "已更新";
                sync_status = "已上传此版本的阅读位置";
            } else {
                uncertain_record = upload_record; uncertain_config = sync_config;
                server_state = status == 401 || status == 403 ? "登录失效" : "未确认";
                remote_state = "待检查";
                sync_status = "尚未确认上传，请检查远端后再决定是否重试。";
            }
            sync_checked = false;
        } else {
            const auto remote = sync::parse_remote(status, bytes);
            server_state = "已连接";
            if (remote.status == sync::RemoteStatus::NoRecord) {
                uncertain_record.reset(); acknowledged_record.reset();
                sync_checked = true; remote_state = "无记录";
                sync_status = "远端没有此书的阅读记录，可上传本机位置。";
            } else if (remote.status == sync::RemoteStatus::Found) {
                uncertain_record.reset(); acknowledged_record.reset();
                Anchor candidate;
                sync_checked = true;
                if (sync::from_xpointer(document, remote.progress, sync_config.dom_version,
                                        candidate, detail)) {
                    remote_anchor = candidate;
                    if (session.saved_record() && session.saved_record()->anchor == candidate) {
                        remote_anchor.reset(); remote_state = "位置相同";
                        sync_status = "本机与远端位于同一源位置。";
                    } else {
                        remote_state = "位置不同";
                        sync_status = "发现远端位置，请选择继续位置。";
                    }
                } else {
                    remote_state = "无法映射";
                    sync_status = memory_error(detail) ? "设备内存不足，暂时无法映射远端位置，未跳转。"
                                                       : "远端位置无法精确映射，未跳转。可继续阅读或明确上传本机位置。";
                }
            } else {
                sync_checked = false; remote_state = "未检查";
                server_state = remote.status == sync::RemoteStatus::Unauthorized ? "登录失效" : "请求失败";
                sync_status = remote.status == sync::RemoteStatus::Unauthorized ? "登录失效，请检查同步配置中的账号凭据。" : "无法读取远端位置，请稍后重试。";
            }
        }
        uploading = false;
        upload_record.reset();
        draw_sync();
        overlay();
    }
    void error(const std::string &detail) {
        abandon_content();
        if (drawing_error) return;
        drawing_error = true;
        // The page offers a save retry only for a failed commit, not for one still pending.
        if (card_available && session.needs_save() && !save_failed)
            persist();
        cancel_sync();
        link_return = confirmed_link_return;
        message = detail;
        std::uint32_t visible_offset;
        const bool available = session.visible_record() && chapter.offset_for(session.visible_record()->anchor, visible_offset);
        const bool retained = (!async_content || available) && !foreground_job && card_available && has_book() && session.visible_record() &&
                              restore_confirmed_position() && panel_background(752);
        view = View::Error;
        hits.clear();
        if (!retained) { canvas.clear(); header("暂时无法继续", 0); }
        else {
            canvas.fill_rect(0, 752, 684, 464, 15);
            canvas.line(0, 752, 683, 752, 1, 3);
        }
        text("暂时无法继续", 36, retained ? 784 : 190, 32, 612, 1, true);
        title_lines(detail, 36, retained ? 850 : 300, 612, 26, false, retained ? 3 : 10);
        if (retained) note("保留最后确认的位置。", 1012);
        button(Back, retained && !session.needs_save() ? "恢复阅读" : "返回", 24, 1080, 306, 96);
        if (session.needs_save()) button(SaveRetry, "重试保存", 354, 1080, 306, 96);
        drawing_error = false;
        if (!display.busy()) overlay();
    }
    void back() {
        if (view == View::Portal) return;
        if (view == View::BookManagement) { draw_details(); overlay(); return; }
        if (usb_prepared) {
            if (usb_confirm) { usb_confirm = false; draw_usb(); overlay(); }
            return;
        }
        if (view == View::RefreshConfirm || view == View::RefreshInterval) {
            draw_refresh(); overlay(); return;
        }
        if (view == View::PowerSettings || view == View::StorageSettings || view == View::InputSettings ||
            view == View::SyncSettings || (view == View::Import && import_from_settings) ||
            (view == View::Refresh && refresh_from_settings)) {
            draw_settings(); overlay(); return;
        }
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
    bool begin_settings_edit(bool global = false) {
        auto snapshot = store.settings_snapshot(global ? "" : book_path);
        if (!snapshot.config.diagnostics.empty()) {
            error("设置文件无效，原文件未修改。请检查后重新打开设置。");
            return false;
        }
        edit_snapshot = std::move(snapshot);
        return true;
    }
    void control(int id) {
        if (view == View::Sync && (id == Contents || id == Typography || id == Refresh)) cancel_sync();
        if (usb_prepared && id != UsbExit && id != UsbExitConfirm && id != UsbCancel)
            return;
        if (view == View::Portal) {
            if (id == PortalStop && !portal_closing) {
                portal_action = PortalAction::Stop;
                portal_closing = true;
                draw_portal(); overlay();
            }
            return;
        }
        if (id == PortalStart) {
            portal_action = PortalAction::Start;
            return;
        }
        if (id == UsbStart) {
            usb_action = UsbTransferAction::Start;
            return;
        }
        if (id == UsbExit || id == UsbCancel) {
            usb_confirm = id == UsbExit;
            draw_usb(); overlay(); return;
        }
        if (id == UsbExitConfirm) {
            usb_action = UsbTransferAction::Exit;
            return;
        }
        if (id >= DetailBase) {
            const auto index = static_cast<std::size_t>(id - DetailBase);
            if (index < entries.size() && !entries[index].directory) {
                inspect_book(entries[index].path); overlay();
            }
            return;
        }
        if (id >= TocToggleBase && id < LinkBase) {
            const auto index = static_cast<std::size_t>(id - TocToggleBase);
            if (index < document.toc().size() && toc_parent(index)) {
                if (!toc_collapsed.erase(index)) toc_collapsed.insert(index);
                draw_contents(); overlay();
            }
            return;
        }
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
            if (async_content) {
                if (!link_return) link_return = origin->anchor;
                clear_page_history();
                begin_chapter(index, 0, ContentJob::Target::Fragment, 0, {}, link.fragment);
                return;
            }
            std::uint32_t target = 0;
            if (!seek_fragment(index, link.fragment, target, "链接锚点不存在"))
                return;
            if (!link_return)
                link_return = origin->anchor;
            clear_page_history();
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
            if (async_content) {
                clear_page_history();
                begin_chapter(target, 0, ContentJob::Target::Fragment, 0, {}, item.fragment);
                return;
            }
            std::uint32_t position = 0;
            if (!seek_fragment(target, item.fragment, position, "目录锚点不存在"))
                return;
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
            } else if (async_content) open(entry.path);
            else
                defer("正在打开书籍…", [this, path = entry.path] { open(path); });
            return;
        }
        switch (id) {
        case LinkReturn: {
            if (!link_return)
                return;
            const auto target = *link_return;
            if (async_content) { link_return.reset(); clear_page_history(); begin_seek(target); return; }
            std::uint32_t target_offset;
            if (!seek(target, target_offset, "原位置无法恢复"))
                return;
            link_return.reset();
            clear_page_history();
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
            save_pages();
            scan_library();
            draw_library();
            overlay(true);
            return;
        case Tools:
            if (has_book()) {
                if (session.visible_record() || session.saved_record())
                    recover_body();
                else
                    open(book_path);
            } else if (!last_book.empty()) {
                if (async_content) open(last_book);
                else defer("正在打开书籍…", [this] { open(last_book); });
            }
            return;
        case Contents: {
            load_fonts();
            toc_page = 0;
            toc_collapsed.clear();
            const auto current = current_toc();
            const auto &toc = document.toc();
            for (std::size_t i = 0; i < toc.size(); ++i) {
                if (!toc_parent(i)) continue;
                auto end = i + 1;
                while (end < toc.size() && toc[end].depth > toc[i].depth) ++end;
                if (current <= i || current >= end) toc_collapsed.insert(i);
            }
            toc_locate_current = true;
            draw_contents();
            break;
        }
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
            refresh_from_settings = view == View::Settings;
            if (!begin_settings_edit(refresh_from_settings))
                return;
            draw_refresh();
            break;
        case LibrarySort:
            library_descending = !library_descending;
            library_page = 0;
            scan_library(); draw_library();
            break;
        case LibraryList:
        case LibraryGrid:
            library_grid = id == LibraryGrid;
            library_page = 0;
            draw_library();
            break;
        case DetailCache:
            covers.erase(std::remove_if(covers.begin(), covers.end(), [&](const auto& cover) { return cover.path == detail_path; }), covers.end());
            if (detail_path == book_path) clear_page_history();
            message = "本书内存缓存已清理，阅读记录保留";
            draw_details();
            break;
        case DetailReset:
        case DetailDelete:
            confirm_management(id == DetailDelete);
            if (view == View::BookManagement) overlay();
            return;
        case ManageConfirm:
            manage_book();
            return;
        case DetailOpen:
            if (async_content) { open(detail_path); return; }
            defer("正在打开书籍…", [this] { open(detail_path); });
            return;
        case RefreshAccept:
            apply_refresh(RefreshFast);
            return;
        case RefreshInterval:
            draft = edit_snapshot.config.settings;
            draw_interval();
            break;
        case IntervalLess:
        case IntervalMore: {
            auto &interval = draft.refresh_preset == "fast" ? draft.fast_full_refresh_pages : draft.balanced_full_refresh_pages;
            interval = std::clamp(interval + (id == IntervalMore ? 1 : -1), 1, 30);
            draw_interval();
            break;
        }
        case IntervalSave:
            save_refresh(draft);
            return;
        case PowerSettings:
            if (!begin_settings_edit(true)) return;
            draft = edit_snapshot.config.settings;
            draw_power();
            break;
        case SleepCover:
        case SleepReading:
        case SleepSimple:
            draft.sleep_screen = id == SleepCover ? "cover" : id == SleepReading ? "reading" : "simple";
            draw_power();
            break;
        case SleepLess:
        case SleepMore:
            draft.auto_sleep_minutes = std::clamp(draft.auto_sleep_minutes + (id == SleepMore ? 5 : -5), 0, 240);
            draw_power();
            break;
        case PowerSave: {
            std::string detail;
            if (store.apply_settings(edit_snapshot, draft, false, detail) != StoreStatus::Ok) {
                error("休眠设置未保存：" + detail); return;
            }
            settings.auto_sleep_minutes = draft.auto_sleep_minutes;
            settings.sleep_screen = draft.sleep_screen;
            message = "休眠设置已保存";
            draw_settings();
            break;
        }
        case StorageSettings:
            message.clear(); draw_storage();
            break;
        case ClearCache:
            abandon_content(); foreground_job.reset(); stop_covers(); stop_raster(); stop_counting_job();
            covers.clear();
            message = store.clear_cache() == StoreStatus::Ok ? "缓存已清理，原书与阅读记录保留" : "缓存未完全清理，请检查 TF 卡";
            draw_storage();
            break;
        case InputSettings:
            message.clear();
            draw_input_settings();
            break;
        case SyncSettings:
            draw_sync_settings();
            break;
        case LibraryView: {
            if (!begin_settings_edit(true)) return;
            auto values = edit_snapshot.config.settings;
            const std::string previous = values.library_view == "grid" ? "卡片" : "列表";
            values.library_view = values.library_view == "grid" ? "list" : "grid";
            std::string detail;
            if (store.apply_settings(edit_snapshot, values, false, detail) == StoreStatus::Ok) {
                settings.library_view = values.library_view;
                library_grid = values.library_view == "grid";
                library_page = 0;
                message.clear();
            } else
                message = "“书库默认视图”未保存，仍按原设置（" + previous + "）生效。请检查存储卡。";
            draw_input_settings();
            break;
        }
        case Settings:
            draw_settings();
            break;
        case Import:
            import_from_settings = view == View::Settings;
            draw_import();
            break;
        case Sync:
            cancel_sync();
            sync_status.clear();
            server_state = "未检查"; remote_state = "未检查";
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
                server_state = "配置变化"; remote_state = "未检查";
                sync_status = "同步配置已更改，请重新检查远端位置";
                draw_sync();
                overlay();
                return;
            }
            const auto target = *remote_anchor;
            if (async_content) {
                const auto origin = session.visible_record();
                cancel_sync();
                if (origin) link_return = origin->anchor;
                clear_page_history(); begin_seek(target); return;
            }
            std::uint32_t start;
            if (!seek(target, start, "远端位置无法恢复"))
                return;
            const auto origin = session.visible_record();
            cancel_sync();
            if (origin) link_return = origin->anchor;
            clear_page_history();
            show_page(start);
            return;
        }
        case FullRefresh:
            force_refresh();
            return;
        case Prev:
        case Next:
            if (view == View::Library) {
                if (id == Prev && library_page)
                    --library_page;
                else if (id == Next && (library_page + 1) * library_page_size() < entries.size())
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
            if (!typography_changed()) return;
            apply_typography();
            return;
        case RefreshClear:
        case RefreshBalanced:
        case RefreshFast:
            if (id == RefreshFast && edit_snapshot.config.settings.refresh_preset != "fast") {
                draw_refresh_confirm(); overlay(); return;
            }
            apply_refresh(id);
            return;
        case SaveRetry:
            if (!persist()) { error(message); return; }
            message = "进度已保存";
            if (has_book()) { recover_body(); return; }
            draw_settings();
            break;
        default:
            return;
        }
        overlay();
    }
    void apply_typography();
    void apply_refresh(int id);
    void save_refresh(const ReaderSettings &values);
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
        release_counting();
        if (content_loading) {
            if (!input.touch && input.event.input == Input::K2) force_refresh();
            else if (!input.queued && input.touch && input.x >= 488 && input.y >= 1140) {
                abandon_content();
                link_return = confirmed_link_return;
                draw_library(); overlay();
            }
            return;
        }
        const bool preserve_prefetch = view == View::Reading &&
            ((!input.touch && (input.event.input == Input::K3 || input.event.input == Input::K2)) ||
             (input.touch && input.x >= 479 && input.y < footer_top));
        if (!preserve_prefetch) clear_prefetch();
        if (input.queued) {
            const bool turn_touch = input.touch && input.y < footer_top && (input.x < 205 || input.x >= 479);
            const bool turn_key = !input.touch && (input.event.input == Input::K1 || input.event.input == Input::K3);
            if (view != View::Reading || !turned || (!turn_touch && !turn_key))
                return;
            // Links are not followed: the contact was aimed at the page that has been replaced.
            turn(turn_touch ? input.x >= 479 : input.event.input == Input::K3);
            return;
        }
        turned = false;
        if (input.touch) {
            if (view == View::Reading) {
                if (input.y >= footer_top) {
                    footer_chapters = !footer_chapters;
                    footer(); overlay(); return;
                }
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
            force_refresh();
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
    clear_page_history();
    reset_pages();
    session.change_layout();
    show_page(offset);
}
void ReaderApp::Impl::apply_refresh(int id) {
    auto values = edit_snapshot.config.settings;
    values.refresh_preset = id == RefreshClear ? "clear" : id == RefreshFast ? "fast" : "balanced";
    save_refresh(values);
}
void ReaderApp::Impl::save_refresh(const ReaderSettings &values) {
    std::string detail;
    if (store.apply_settings(edit_snapshot, values, !refresh_from_settings, detail) != StoreStatus::Ok) {
        error("刷新设置未保存：" + detail);
        return;
    }
    // Refresh edits do not apply external typography changes to the visible page.
    const auto effective = store.load_settings(book_path).settings;
    settings.refresh_preset = effective.refresh_preset;
    settings.balanced_full_refresh_pages = effective.balanced_full_refresh_pages;
    settings.fast_full_refresh_pages = effective.fast_full_refresh_pages;
    edit_snapshot = store.settings_snapshot(refresh_from_settings ? "" : book_path);
    draw_refresh();
    overlay();
}

ReaderApp::ReaderApp(std::string root, std::string writer, bool controlled_storage, bool async_content)
    : impl_(std::make_unique<Impl>(std::move(root), std::move(writer))) { impl_->controlled_storage = controlled_storage; impl_->async_content = async_content; }
void ReaderApp::invalidate_identity_cache() { impl_->drain_save(); impl_->abandon_content(); impl_->foreground_job.reset(); impl_->stop_covers(); impl_->stop_raster(); impl_->stop_counting_job(); impl_->identities.clear(); impl_->font_identity.clear(); impl_->effective_font.clear(); }
ReaderApp::~ReaderApp() { impl_->drain_save(); }
bool ReaderApp::initialize() {
    return impl_->initialize();
}
bool ReaderApp::open_book(const std::string &path) {
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    return !impl_->usb_prepared && impl_->open(path);
}
void ReaderApp::set_battery(std::optional<int> percent) {
    impl_->battery_percent = percent && *percent >= 0 && *percent <= 100 ? percent : std::nullopt;
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
        if (frame.purpose == FramePurpose::Body) impl_->remember_page();
        impl_->confirmed_link_return = impl_->link_return;
        impl_->save_failed = false;
        impl_->recent_save_failed = false;
        if (!impl_->async_content && impl_->last_recorded != impl_->book_path) {
            std::string error;
            if (impl_->store.save_last_book(impl_->book_path, error) != StoreStatus::Ok)
                impl_->message = "最近阅读记录未保存";
            else
                impl_->last_recorded = impl_->book_path;
        }
    }
    if (success && impl_->view == View::Tools && std::exchange(impl_->tools_save_pending, false)) {
        impl_->schedule_save();
    }
    if (auto work = std::exchange(impl_->deferred, nullptr); work && success)
        work();
    if (!success) {
        impl_->abandon_content();
        if (impl_->usb_prepared) {
            impl_->message = "屏幕更新失败，请退出并重启。";
            impl_->draw_usb(); impl_->overlay(true);
        } else impl_->error("屏幕更新失败，当前页未保存。返回将恢复已确认位置。");
    }
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
UsbTransferAction ReaderApp::take_usb_transfer_action() {
    return std::exchange(impl_->usb_action, UsbTransferAction::None);
}
bool ReaderApp::prepare_usb_transfer() {
    invalidate_identity_cache();
    if (impl_->usb_prepared || impl_->display.busy() || impl_->active_network ||
        !impl_->card_available || !impl_->persist()) return false;
    impl_->cancel_sync();
    impl_->save_pages();
    if (!impl_->ui.open_fallback()) return false;
    impl_->document.close();
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    impl_->body.open_fallback();
    release(impl_->chapter);
    impl_->page = {};
    impl_->links.clear();
    impl_->entries.clear();
    impl_->clear_page_history();
    impl_->covers.clear();
    impl_->card_available = false;
    impl_->usb_prepared = true;
    return true;
}
void ReaderApp::show_usb_transfer(bool active, const std::string& error) {
    impl_->message = error;
    impl_->usb_active = active;
    impl_->usb_confirm = false;
    if (impl_->usb_prepared) impl_->draw_usb();
    else impl_->draw_import();
    impl_->overlay(true);
}
PortalAction ReaderApp::take_portal_action() {
    return std::exchange(impl_->portal_action, PortalAction::None);
}
bool ReaderApp::prepare_portal() {
    if (impl_->content_loading || impl_->usb_prepared || impl_->display.busy() || impl_->active_network ||
        !impl_->card_available || !impl_->persist()) return false;
    impl_->cancel_sync();
    impl_->clear_body_cache();
    impl_->stop_covers();
    impl_->stop_raster();
    impl_->release_counting();
    impl_->portal = {};
    impl_->portal_closing = false;
    impl_->draw_portal();
    impl_->overlay();
    return true;
}
void ReaderApp::show_portal(const PortalStatus &status) {
    if (impl_->usb_prepared || impl_->display.busy()) return;
    const auto view = impl_->view;
    if (status.phase == PortalPhase::Closed) {
        // A refused start arrives while the requesting page is still shown.
        if (view != View::Portal && view != View::Import && view != View::SyncSettings) return;
        impl_->refresh_badge_config();
        impl_->portal = {};
        impl_->portal_closing = false;
        impl_->message = status.detail;
        impl_->draw_settings();
        impl_->overlay();
        return;
    }
    if (view != View::Portal) return;
    // The codes must be scannable under every refresh preset, so their first frame clears the panel.
    const bool codes_appear = impl_->portal.access_ssid.empty() && !status.access_ssid.empty();
    impl_->portal = status;
    impl_->draw_portal();
    impl_->overlay(codes_appear);
}
bool ReaderApp::prepare_sleep() {
    invalidate_identity_cache();
    if (impl_->usb_prepared || impl_->view == View::Portal) return false;
    if (impl_->display.busy() || impl_->active_network || !impl_->persist())
        return false;
    if (impl_->has_book() && !impl_->session.visible_record()) return false;
    impl_->save_pages();
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    const auto sync_state = impl_->sync_badge();
    impl_->cancel_sync();
    impl_->draw_sleep(sync_state);
    if (impl_->view != View::Sleeping) return false;
    impl_->overlay(true);
    return true;
}
void ReaderApp::wake() {
    if (impl_->display.busy())
        return;
    if (impl_->has_book()) {
        if (impl_->async_content) impl_->open(impl_->book_path);
        else impl_->recover_body();
    } else {
        impl_->draw_library();
        impl_->overlay(true);
    }
}
void ReaderApp::card_removed() {
    invalidate_identity_cache();
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    if (impl_->usb_prepared) { show_usb_transfer(false, "TF 卡已移除，请退出并重启。"); return; }
    impl_->cancel_sync();
    impl_->covers.clear();
    impl_->card_available = false;
    impl_->ui.open_fallback();
    impl_->body.open_fallback();
    impl_->effective_font.clear();
    impl_->error("TF 卡已移除。请重新插卡并重启设备。");
}
bool ReaderApp::save_due() const {
    return !impl_->usb_prepared && !impl_->display.busy() && (impl_->session.needs_save() ||
           (impl_->async_content && !impl_->recent_save_failed && impl_->session.visible_record() && impl_->last_recorded != impl_->book_path)) &&
           !impl_->save_failed && !impl_->save_job;
}
void ReaderApp::save_progress() {
    if (impl_->usb_prepared || impl_->display.busy()) return;
    impl_->schedule_save();
}
bool ReaderApp::save_busy() const { return !!impl_->save_job; }
std::optional<BookPages> ReaderApp::book_pages() const {
    const auto page = impl_->has_book() ? impl_->current_page() : 0;
    return page ? std::optional<BookPages>{{page, impl_->total_pages()}} : std::nullopt;
}
CacheUsage ReaderApp::cache_usage() const {
    CacheUsage usage;
    usage.body_bitmap = impl_->body_canvas ? impl_->body_canvas->pixels().capacity() : 0;
    if (impl_->next_page) usage.next_layout = impl_->next_page->glyphs.capacity() * sizeof(graphics::PositionedGlyph) +
        impl_->next_page->lines.capacity() * sizeof(graphics::TextLine);
    usage.next_bitmap = impl_->next_bitmap ? impl_->next_bitmap->pixels().capacity() : 0;
    usage.bitmap_hits = impl_->bitmap_hits;
    usage.next_content = impl_->next_chapter ? chapter_cache_bytes(*impl_->next_chapter) : 0;
    usage.page_index = impl_->page_index_bytes();
    for (const auto& cover : impl_->covers) usage.cover_pixels += cover.pixels.capacity();
    usage.ui_glyph_bytes = impl_->ui.cached_glyph_bytes(); usage.body_glyph_bytes = impl_->body.cached_glyph_bytes();
    usage.ui_glyphs = impl_->ui.cached_glyphs(); usage.body_glyphs = impl_->body.cached_glyphs();
    usage.ui_advances = impl_->ui.cached_advances(); usage.body_advances = impl_->body.cached_advances();
    return usage;
}
bool ReaderApp::content_busy() const { return !!impl_->foreground_job || impl_->pending_content.has_value(); }
bool ReaderApp::background_due() const {
    return !!impl_->cover_job || impl_->pending_covers.has_value() || !!impl_->raster_job || impl_->prefetch_due() || impl_->counting_due();
}
const char *ReaderApp::background_state() const {
    if (impl_->content_loading) {
        const auto kind = impl_->pending_content ? impl_->pending_content->kind : impl_->foreground_job->request.kind;
        return kind == ContentJob::Kind::Open ? "opening" : kind == ContentJob::Kind::Chapter ? "chapter" : "image";
    }
    if (impl_->cover_job || impl_->pending_covers) return "covers";
    if (impl_->raster_job || impl_->prefetch_due()) return "prefetch";
    if (impl_->pages_cache_pending || impl_->pages_dirty) return "page-cache";
    if (!impl_->has_book() || impl_->pages_complete) return "idle";
    if (impl_->pages_failed) return "failed";
    return impl_->counting_blocked == impl_->chapter_index ? "waiting" : "counting";
}
void ReaderApp::poll_background(bool cancel) {
    if (cancel && impl_->counting_job) impl_->counting_job->cancel();
    impl_->poll_raster();
    impl_->poll_counting_job();
    impl_->poll_covers();
    impl_->poll_save();
    if (impl_->save_error_pending && !impl_->display.busy() && !impl_->content_loading) {
        impl_->save_error_pending = false; impl_->error(impl_->message);
    }
    if (!cancel || !impl_->content_loading) impl_->poll_content();
}
void ReaderApp::background_step(const std::function<bool()>& stop) {
    impl_->poll_raster();
    impl_->poll_counting_job();
    if (stop && stop()) { if (impl_->counting_job) impl_->counting_job->cancel(); return; }
    if (impl_->prefetch_due()) impl_->prepare_next(stop);
    else if (impl_->counting_due()) impl_->count_pages(stop);
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
    if (!impl_->usb_prepared) impl_->network_complete(id, status, body, error);
}
} // namespace pico_page
