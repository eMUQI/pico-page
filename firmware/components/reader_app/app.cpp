#include "content_cache.h"
#include "content_job.h"
#include "help_content.h"
#include "reading_footer.h"
#include "pico_page/stage_timer.h"
#include "pico_page/app.h"
#include "pico_page/document.h"
#include "pico_page/provision.h"
#include "pico_page/store.h"
#include "pico_page/update.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <functional>
#include <map>
#include <set>
#include <sys/stat.h>
#include <utility>
#ifdef ESP_PLATFORM
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

namespace pico_page {
namespace {
using graphics::decode_utf8;
constexpr int footer_top = 1128, toolbar_top = 1080;
constexpr char kProjectHome[] = "picopage.wulu.zone";
constexpr int viewer_left = 24, viewer_top = 104, viewer_width = 636, viewer_height = 1088;
constexpr int viewer_toolbar_height = 96, viewer_action_left = 456;
constexpr int viewer_contrast_levels = 3;
constexpr auto inline_image_delay = std::chrono::milliseconds(250);
constexpr const char *content_revision = "epub-native-v1";
constexpr const char *txt_revision = "txt-utf8-v1";
struct Entry {
    std::string path, name;
    bool directory;
    // Status line of the library row; filled when the row is first drawn.
    std::string status = {};
    bool described = false;
    std::size_t recent_rank = 128;
    std::int64_t opened_at = 0;
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
    FontWeight,
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
    LibrarySort, LibraryFolders,
    LibraryList,
    LibraryGrid,
    RefreshAccept,
    RefreshInterval,
    PageEffects, EffectOff, EffectA, EffectB, EffectC, EffectRipple,
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
    WifiUpload,
    WifiUploadStation,
    PortalStop,
    WifiSettings,
    WifiToggle,
    Help, About,
    HelpPrevious, HelpTools, HelpNext, HelpPosition, HelpDevice,
    HelpKey1, HelpKey2, HelpKey3, ResumeDetails,
    SyncCancel,
    Retry,
    PowerMenu,
    PowerShutdown,
    PowerRestart,
    PowerCancel,
    PowerForce,
    BookmarkToggle,
    ContentsToc,
    ContentsBookmarks,
    EntryBase = 100,
    TocBase = 1000,
    TocToggleBase = 10000,
    LinkBase = 20000,
    DetailBase = 30000,
    BookmarkBase = 40000,
    WifiConnect = 50000, WifiDisconnect, WifiManage, WifiForget, WifiForgetConfirm,
    WifiModeOpen, WifiModeAuto, WifiAdd, WifiRescan, WifiManualName, WifiUpdatePassword,
    WifiJoinCancel, WifiJoinRetry, WifiJoinEdit, WifiJoinList, WifiJoinDone,
    SyncEnable, SyncOnceAuto, SyncAccount, SyncStay, ContinueOffline,
    AccountEndpoint, AccountUser, AccountPassword, AccountVerify, AccountCancel, AccountRemove, AccountRemoveConfirm,
    EntryClear, EntryMask, EntryDefault,
    WifiSavedBase = 50100, WifiModeBase = 50200, WifiNearbyBase = 50300,
    SleepCustom = 51000, SleepRotation, SleepChoose, SleepRefresh, LockScreenBase = 52000,
    UpdateOpen = 53000, UpdateCheck, UpdateNotes, UpdateStart, UpdateConfirm, UpdateCancel,
    UpdateRetry, UpdateLater, UpdateRestart, UpdateDismiss
};
static_assert(ContentsBookmarks < EntryBase);
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
bool txt_name(std::string name) {
    for (auto &c : name)
        if (c >= 'A' && c <= 'Z') c += 32;
    return name.size() > 4 && name.substr(name.size() - 4) == ".txt";
}
bool book_name(std::string name) {
    for (auto &c : name)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return (name.size() > 5 && name.substr(name.size() - 5) == ".epub") ||
           (name.size() > 4 && name.substr(name.size() - 4) == ".txt");
}
const char* revision_for(const std::string& path, const DocumentMetadata& metadata) {
    return txt_name(path) ? (metadata.txt_gb18030 ? kTxtGb18030Revision : txt_revision) : content_revision;
}
/**
 * Single-line display text of at most limit scalars: control and line-separator characters
 * become one space, runs of spaces collapse, and surrogates or out-of-range values are dropped.
 */
std::string snippet(std::u32string_view text, std::size_t limit) {
    std::string result;
    std::size_t count = 0;
    bool space = false;
    for (char32_t c : text) {
        if (count == limit) break;
        if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) continue;
        if (c < 0x20 || c == 0x7f || c == U' ' || c == 0x3000 || c == 0x2028 || c == 0x2029 || c == 0xa0) {
            space = !result.empty();
            continue;
        }
        if (space) { result += ' '; ++count; space = false; if (count == limit) break; }
        if (c < 0x80) result += static_cast<char>(c);
        else if (c < 0x800) { result += static_cast<char>(0xc0 | (c >> 6)); result += static_cast<char>(0x80 | (c & 0x3f)); }
        else if (c < 0x10000) {
            result += static_cast<char>(0xe0 | (c >> 12)); result += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (c & 0x3f));
        } else {
            result += static_cast<char>(0xf0 | (c >> 18)); result += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
            result += static_cast<char>(0x80 | ((c >> 6) & 0x3f)); result += static_cast<char>(0x80 | (c & 0x3f));
        }
        ++count;
    }
    return result;
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
    // Whether body_canvas shows the bookmark ribbon; reuse updates it without replacing body pixels.
    bool body_bookmarked = false;
    std::unique_ptr<graphics::Canvas> viewer_original;
    bool image_viewing = false;
    int image_contrast_level = 0;
    graphics::LayoutSpec body_spec;
    std::vector<Hit> body_links;
    void clear_body_cache() {
        body_canvas.reset(); body_links.clear(); viewer_original.reset();
        image_viewing = false; image_contrast_level = 0;
    }
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
    std::unique_ptr<graphics::Canvas> next_bitmap;
    std::uint64_t bitmap_hits = 0;
    graphics::LayoutSpec next_spec;
    std::size_t warm_cursor = 0;
    bool prefetch_done = false;
    void clear_prefetch() {
        next_bitmap.reset();
        next_page.reset();
        warm_cursor = 0;
        prefetch_done = false;
    }
    bool prefetch_due() const {
        return !sleep_cover_deadline && !power_menu_wait && !session.needs_save() && view == View::Reading && !image_viewing && has_book() && card_available && !usb_prepared &&
               !display.busy() && !deferred && !save_job && !bookmark_job && !foreground_job && !pending_content && !body_images_pending && !prefetch_done;
    }
    void prepare_next(const std::function<bool()>& stop) {
        if (stop && stop()) return;
        const StageTimer timer{"next_page_prepare"};
        if (!next_page) {
            const auto start = readable_offset(page.end);
            // The next part or chapter is loaded by the turn that reaches it.
            if (start >= chapter.text.size()) { prefetch_done = true; return; }
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
        // Images are prepared only for a displayed page; the planned layout is kept for the turn.
        if (!next_page->images.empty()) { prefetch_done = true; return; }
        if (!body.size(next_page->font_px)) { next_page.reset(); prefetch_done = true; return; }
        int warm_size = next_page->font_px;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(15);
        while (warm_cursor < next_page->glyphs.size()) {
            if ((stop && stop()) || std::chrono::steady_clock::now() >= deadline) return;
            const auto& glyph = next_page->glyphs[warm_cursor];
            if ((glyph.font_px != warm_size && !body.size(glyph.font_px)) ||
                !body.warm(glyph.scalar, glyph.bold)) {
                next_page.reset(); prefetch_done = true; return;
            }
            warm_size = glyph.font_px;
            ++warm_cursor;
        }
        // A text page is drawn here, after its glyphs are warm, because the glyph cache belongs to
        // this task; a cancelled attempt is discarded and repeated in a later step.
        if (!next_bitmap && body_cache_room()) {
            const StageTimer timer{"next_page_bitmap"};
            auto bitmap = std::make_unique<graphics::Canvas>();
            if (graphics::render_page(*bitmap, body, *next_page, 1, stop)) next_bitmap = std::move(bitmap);
            else if (stop && stop()) return;
        }
        prefetch_done = true;
    }
    ReaderSettings settings, draft;
    std::size_t help_page = 0;
    int help_zone = HelpTools, help_key = HelpKey2, help_position = 0, help_device = 0;
    SettingsSnapshot edit_snapshot;
    View view = View::Library;
    std::optional<graphics::Canvas> power_return_canvas;
    std::vector<Hit> power_return_hits;
    View power_return_view = View::Library;
    std::optional<View> power_menu_wait;
    std::string power_return_message;
    PowerAction power_selected = PowerAction::None;
    PowerRequest power_request;
    bool power_save_failed = false;
    bool power_progress_saved = false;
    void draw_power_menu() {
        view = View::PowerMenu;
        hits.clear(); canvas.clear(); header("电源", 0);
        if (!power_save_failed) {
            text("关机或重启", 36, 184, 36, 612, 1, true);
            wrapped("菜单出现后松开电源键。选择后保存本地阅读位置，不等待云端同步。", 36, 276, 612, 26);
            button(PowerShutdown, "关机", 24, 480, 636, 96);
            button(PowerRestart, "重启", 24, 612, 636, 96);
            button(PowerCancel, "取消", 24, 1080, 636, 96);
        } else {
            const bool restart = power_selected == PowerAction::Restart;
            text("本地保存未完成", 36, 184, 36, 612, 1, true);
            wrapped(message, 36, 276, 612, 26);
            wrapped("继续可能丢失最后一段阅读进度。", 36, 456, 612, 26);
            button(PowerCancel, "返回", 24, 1080, 306, 96);
            button(PowerForce, restart ? "仍然重启" : "仍然关机", 354, 1080, 306, 96, true);
        }
    }
    void close_power_menu() {
        if (!power_return_canvas) return;
        canvas = std::move(*power_return_canvas); power_return_canvas.reset();
        hits = std::move(power_return_hits); view = power_return_view;
        message = power_return_message; power_selected = PowerAction::None;
        power_save_failed = false; power_request = {};
        if (view == View::Library) draw_library();
        overlay();
    }
    void draw_power_off(const std::string& detail, bool manual = false) {
        view = View::PowerOff;
        message = detail;
        hits.clear(); canvas.clear(); header("电源", 0);
        text(manual ? "请手动关机" : power_selected == PowerAction::Restart ? "正在重启" : "已关机",
             36, 240, 40, 612, 1, true);
        wrapped(detail, 36, 360, 612, 28);
    }
    bool resume_to_library = false;
    bool usb_prepared = false, usb_active = false, usb_confirm = false;
    UsbTransferAction usb_action = UsbTransferAction::None;
    std::optional<View> usb_request_view;
    WifiStatus wifi_status;
    // manual_hold keeps the station up after "connect now"; wifi_hold cancels one scan or join.
    network::Hold manual_hold, sync_hold, wifi_hold, account_hold;
    std::optional<WifiTargets> manual_target, sleep_wifi_target;
    std::optional<WifiOperation> wifi_request;
    std::uint64_t wifi_sequence = 0, wifi_worker = 0;
    WifiOperation::Kind wifi_kind = WifiOperation::Kind::Connect;
    std::vector<provision::SavedWifi> wifi_saved;
    std::vector<provision::NearbyWifi> wifi_nearby;
    // wifi_target is the pinned network name; empty selects automatic connection.
    std::string wifi_target, wifi_notice, wifi_choice, pending_wifi_record;
    enum class WifiPage { Home, Mode, Saved, Detail, Forget, Add, Joining, JoinFailed, Joined };
    // wifi_root is the page that was opened from outside; back on it leaves the Wi-Fi pages.
    WifiPage wifi_page = WifiPage::Home, wifi_root = WifiPage::Home;
    bool wifi_from_sync = false, wifi_from_transfer = false, wifi_scanned = false, account_from_sync = false;
    std::size_t wifi_add_page = 0;
    // Credentials of the network being added or updated. They stay in RAM until the join succeeds.
    provision::WifiCredentials join_draft;
    // join_open: an open network picked from the scan list, which has no password page.
    // join_secured: a protected network picked from the list, which cannot take an empty password.
    bool join_manual = false, join_open = false, join_secured = false;
    // The station link belongs to a join until the radio reports offline; it earns no connection record.
    bool join_link = false;
    WifiPage join_origin = WifiPage::Add;
    LinkFailure join_failure = LinkFailure::None;
    std::string join_error;
    /** One on-screen keyboard field. Key taps change this model; flush_entry() draws it. */
    struct TextEntry {
        enum class Field { None, WifiPassword, WifiName, Endpoint, Username, Password };
        Field field = Field::None;
        std::string value;
        bool masked = false, shift = false;
        int layer = 0;  // 0 letters, 1 digits and punctuation, 2 remaining symbols.
        bool field_dirty = false, keys_dirty = false;
        bool ready_drawn = true;  // The enter key was last drawn as available.
    } entry;
    /** Unsaved KOSync account fields. The password is kept in RAM only until it is verified. */
    struct AccountDraft {
        std::string endpoint, username, password;
        bool saved = false;    // sync.toml held a valid account when the page was opened.
        bool changed = false;  // A field differs from that account.
        // Outcome of the last check of the saved account in this visit; a saved file alone proves neither.
        enum class Check { None, Accepted, Rejected } check = Check::None;
    } account;
    enum class AccountPage { Form, Verifying, Remove };
    AccountPage account_page = AccountPage::Form;
    std::optional<sync::Config> account_check;
    std::string account_notice;
    PortalAction portal_action = PortalAction::None;
    PortalStatus portal;
    bool portal_closing = false;
    bool portal_live = false, portal_requested = false, portal_open_wifi = false;
    std::uint64_t portal_generation = 0;
    View portal_request_view = View::Import;
    // The upload session runs over a saved network rather than the device access point.
    bool portal_station = false;
    // A Waiting state arrived; a session closing before it never offered its page.
    bool portal_started = false;
    std::optional<Frame> pending;
    std::optional<NetworkOperation> network_request;
    std::shared_ptr<std::atomic_bool> network_cancel;
    std::optional<Anchor> remote_anchor;
    // Where remote_anchor lies in its chapter; set by the mapping that produced the anchor.
    std::optional<sync::PartPosition> remote_place;
    std::optional<ReadingRecord> upload_record, acknowledged_record, uncertain_record;
    sync::Config acknowledged_config, uncertain_config;
    enum class SyncState { Unchecked, NoRecord, Same, Different, Unmapped, Uploaded, Uncertain,
                           Unauthorized, Failed, LinkFailed, ConfigChanged };
    SyncState sync_state = SyncState::Unchecked;
    // Percentage in [0, 1] as the server reported it, and device name, from the last answered check;
    // negative when unknown. Each client computes this percentage its own way, so it is shown only
    // for a position that could not be mapped and never compared with a local percentage.
    double remote_percent = -1;
    std::string remote_device;
    // Whether the running or failed check may use only the pinned network, and that network's name.
    bool sync_link_pinned = false;
    std::string sync_link_name;
    sync::Config sync_config, badge_config;
    bool badge_configured = false, tools_save_pending = false;
    void refresh_badge_config() {
        std::string detail;
        badge_configured = sync::load_config(root, badge_config, detail);
    }
    std::uint64_t network_sequence = 0, active_network = 0;
    // Keep the dispatch slot until its matching completion, even after UI cancellation.
    std::uint64_t network_worker_id = 0;
    int network_after_cancel = 0;
    View network_return_view = View::Sync;
    bool sync_checked = false, uploading = false;
    std::string sync_status;
    std::vector<Entry> entries;
    struct CoverThumbnail {
        std::string path;
        off_t size;
        time_t modified;
        std::vector<std::uint8_t> pixels;
    };
    // One screen shows six thumbnails; eight hold 184,000 bytes of packed pixels.
    static constexpr std::size_t kCoverSlots = 8;
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
                    if (covers.size() == kCoverSlots) covers.erase(covers.begin());
                    covers.push_back({source.path, source.size, source.modified, std::move(job->result.covers[i])});
                }
                draw_library(); overlay();
            }
        }
        if (!pending_covers || display.busy() || save_job || bookmark_job || foreground_job || pending_content || counting_job) return;
        cover_job = std::make_unique<ContentJob>(std::move(*pending_covers)); pending_covers.reset();
        if (!cover_job->start()) {
            cover_job.reset(); message = "封面暂未生成，可继续阅读";
            for (const auto& source : missing_covers) {
                if (covers.size() == kCoverSlots) covers.erase(covers.begin());
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
    bool toc_locate_current = false, library_grid = false;
    std::string library_sort = "recent";
    bool library_resume_omitted = false, folders_first = true, library_recent = true;
    std::int64_t resume_opened_at = 0;
    // Bookmarks of the open book, loaded with it. Only Ok and Missing lists are shown.
    BookmarkList bookmarks;
    // The contents panel lists bookmarks instead of the table of contents until another book opens.
    bool contents_bookmarks = false;
    std::size_t bookmark_page = 0;
    // Sorted chapter offsets of the bookmarks in the loaded part, rebuilt when either changes.
    mutable std::vector<std::uint32_t> bookmark_offsets;
    mutable std::string bookmark_offsets_resource;
    mutable std::uint32_t bookmark_offsets_part = 0;
    mutable std::uint64_t bookmark_offsets_version = 0;
    std::uint64_t bookmark_version = 1;
    bool bookmarks_usable() const {
        return bookmarks.status == StoreStatus::Ok || bookmarks.status == StoreStatus::Missing;
    }
    void load_bookmarks(BookmarkList* loaded = nullptr) {
        bookmarks = loaded ? std::move(*loaded) :
            store.load_bookmarks(book_path, document.metadata().book_id, revision_for(book_path, document.metadata()));
        ++bookmark_version;
        bookmark_page = 0;
    }
    /** True when a usable bookmark maps into [start, end) of the loaded chapter part. */
    bool bookmarked(std::size_t start, std::size_t end) const {
        if (!bookmarks_usable() || bookmarks.bookmarks.empty()) return false;
        if (bookmark_offsets_version != bookmark_version || bookmark_offsets_resource != chapter.resource ||
            bookmark_offsets_part != chapter.part) {
            bookmark_offsets.clear();
            for (const auto& bookmark : bookmarks.bookmarks) {
                std::uint32_t position;
                if (chapter.offset_for(bookmark.anchor, position)) bookmark_offsets.push_back(position);
            }
            std::sort(bookmark_offsets.begin(), bookmark_offsets.end());
            bookmark_offsets_version = bookmark_version;
            bookmark_offsets_resource = chapter.resource;
            bookmark_offsets_part = chapter.part;
        }
        const auto first = std::lower_bound(bookmark_offsets.begin(), bookmark_offsets.end(), start);
        return first != bookmark_offsets.end() && *first < end;
    }
    /** Indices of the bookmarks inside the visible page. */
    std::vector<std::size_t> page_bookmarks() const {
        std::vector<std::size_t> found;
        if (!bookmarks_usable()) return found;
        for (std::size_t i = 0; i < bookmarks.bookmarks.size(); ++i) {
            std::uint32_t position;
            if (chapter.offset_for(bookmarks.bookmarks[i].anchor, position) && position >= page.start && position < page.end)
                found.push_back(i);
        }
        return found;
    }
    /** A 24-pixel-wide bookmark ribbon centred on cx with a notched lower edge; outlined unless filled. */
    void bookmark_ribbon(int cx, int top, int height, bool filled) {
        constexpr int half = 12, notch = 12;
        for (int y = 0; y < height; ++y) {
            const int cut = y < height - notch ? -1 : y - (height - notch);
            for (int x = -half; x < half; ++x) {
                const bool inside = cut < 0 || x < -cut || x >= cut;
                const bool edge = y < 3 || x < -half + 3 || x >= half - 3 ||
                                  (cut >= 0 && ((x >= -cut - 3 && x < -cut) || (x >= cut && x < cut + 3)));
                if (inside && (filled || edge)) canvas.pixel(cx + x, top + y, 1);
            }
        }
    }
    std::string bookmark_problem() const {
        switch (bookmarks.status) {
        case StoreStatus::Ok: case StoreStatus::Missing: return {};
        case StoreStatus::IdentityMismatch:
            return "书签文件与当前原书不符，未使用其中的书签。原文件已保留；本书加书签时，旧文件改名后保留在同一目录。";
        case StoreStatus::Invalid:
            return "书签文件无法读取，已保留原文件，暂不能添加或删除书签。阅读进度不受影响。";
        case StoreStatus::UnsupportedVersion:
            return "书签文件由较新的固件写入，本固件无法读取，原文件未修改。阅读进度不受影响。";
        default:
            return "书签文件读取失败，请检查 TF 卡后重新打开本书。阅读进度不受影响。";
        }
    }
    // One bookmark commit at a time. In asynchronous mode it runs on a content worker, never
    // concurrently with a progress commit, and completes on this task through poll_bookmark.
    std::unique_ptr<ContentJob> bookmark_job;
    bool bookmark_adding = false;
    void toggle_bookmark() {
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
            // The synchronous mode also commits progress on this task.
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
    /** Starts a queued commit once no other card writer or worker runs, and adopts a finished one. */
    void poll_bookmark(bool redraw = true) {
        if (!bookmark_job || !async_content) return;
        if (!bookmark_job->started()) {
            if ((save_job && save_job->started()) || counting_job || cover_job ||
                (foreground_job && foreground_job->started() && !foreground_job->done())) return;
            if (!bookmark_job->start()) {
                bookmark_job->result.bookmark_status = StoreStatus::IoError;
                if (!finish_bookmark(redraw) && !redraw) { bookmark_drain_failed = true; bookmark_drain_message = message; }
            }
            return;
        }
        if (!bookmark_job->done() || (redraw && display.busy())) return;
        finish_bookmark(redraw);
    }
    // A commit drained before a card handoff failed: the next persist reports it once, a later successful commit clears it.
    bool bookmark_drain_failed = false;
    std::string bookmark_drain_message;
    // A failed commit that finished on a page without a notice line; poll_background shows it.
    std::string bookmark_error;
    /**
     * Waits for a queued or running commit before card files are released or another card
     * writer runs on this task. Draws nothing.
     */
    void drain_bookmark() {
        if (!bookmark_job) return;
        if (!bookmark_job->started()) {
            stop_covers(); clear_prefetch(); stop_counting_job();
            if (foreground_job && foreground_job->started()) foreground_job->wait();
            if (save_job && save_job->started()) { save_job->wait(); poll_save(); }
            poll_bookmark(false);
        }
        if (bookmark_job) {
            bookmark_job->wait();
            if (!finish_bookmark(false)) { bookmark_drain_failed = true; bookmark_drain_message = message; }
        }
    }
    /** Serializes panel file access and reports a drained failure before another operation. */
    bool bookmark_io_ready() {
        drain_bookmark();
        if (!bookmark_drain_failed) return true;
        bookmark_drain_failed = false;
        bookmark_error.clear();
        error(bookmark_drain_message);
        return false;
    }
    /** Adopts a finished commit; false when the requested change was not stored. */
    bool finish_bookmark(bool redraw) {
        auto job = std::move(bookmark_job);
        const auto status = job->result.bookmark_status;
        // A result for a book that is no longer open stays in that book's file only.
        if (job->request.relative_path != book_path || job->request.identity.book_id != document.metadata().book_id)
            return status == StoreStatus::Ok;
        if (status == StoreStatus::Ok) bookmark_drain_failed = false;
        if (status == StoreStatus::Ok || status == StoreStatus::ExternalChange) {
            // Cached pages update the ribbon on reuse without discarding rendered images.
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
            // Other pages have no notice line; a failure becomes an error page on the next poll.
            else if (status != StoreStatus::Ok) bookmark_error = message;
        }
        return status == StoreStatus::Ok;
    }
    // A single configured root is presented as the library home instead of as a folder entry.
    std::string library_home_root;
    // The open folder is one of the configured library roots; its parent is the library.
    bool folder_root = false;
    bool library_directory_unavailable = false;
    reading_footer::Position footer_position = reading_footer::Position::Pages;
    reading_footer::Device footer_device = reading_footer::Device::Battery;
    PageEffect page_effect = PageEffect::Off;
    bool refresh_from_settings = false, import_from_settings = false, help_from_settings = false;
    std::string detail_path, detail_position, detail_error;
    BookSnapshot management_snapshot;
    bool delete_original = false, drawing_error = false;
    DocumentMetadata detail_metadata;
    std::unique_ptr<graphics::Canvas> detail_cover;
    ProgressResult detail_progress;
    std::optional<std::uint64_t> detail_size;
    std::optional<int> battery_percent;
    std::uint64_t ui_sequence = 0;
    int parameter = FontSize;
    bool book_scope = true, card_available = true, font_fallback = false, builtin_body = false, weight_unavailable = false,
         reset_overrides = false;
    unsigned body_updates = 0;
    std::string effective_font, effective_weight, last_recorded;
    std::vector<std::string> fonts;
    std::function<void()> deferred;
    bool save_failed = false, notice_shown = false;
    // The most recent accepted input turned a page; see AppInput::queued.
    bool turned = false;
    // Reading status notices about the body font; the status line shows the position otherwise.
    static constexpr const char *kFallbackFontNotice = "所选字体不可用，已使用替代字体";
    static constexpr const char *kWeightUnavailableNotice = "所选字重不可用，保留字体原有字重";
    static constexpr const char *kBuiltinFontNotice = "未找到正文字体，已使用内置字体";
    static constexpr const char *kMissingNotice = "部分字符无法显示，可在排版中更换字体";
    static constexpr const char *kSaveFailed = "进度未保存，请检查 TF 卡后重试";
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
    std::shared_ptr<FileDigest> font_scan;
    bool font_scan_turn = true, pages_cache_disabled = false;
    std::string body_font_path() const {
        return builtin_body ? "" : root + "/" +
            (font_fallback ? "Fonts/LXGWWenKai-Regular.ttf" : effective_font);
    }
    // The synchronous fallback releases its extra chapter on input. The worker owns a separate
    // chapter until it observes cancellation; both paths retain accepted pagination offsets.
    Chapter counting_chapter;
    bool async_content = false;
    std::uint64_t counting_generation = 0;
    // Kept between pagination jobs while reading. It holds the book and body font open and one
    // chapter part in memory, so it is dropped whenever other work needs the files or the memory.
    std::shared_ptr<PaginationSession> counting_session;
    /** Discards results of running counting jobs and the preparation they share. */
    void invalidate_counting() { ++counting_generation; counting_session.reset(); }
    /** Ends the counting session for foreground work; true when there was one to end. */
    bool yield_counting_session() {
        if (!counting_session) return false;
        if (counting_job && counting_job->request.session) counting_job->cancel();
        counting_session.reset();
        return true;
    }
    std::unique_ptr<ContentJob> counting_job;
    std::unique_ptr<ContentJob> foreground_job;
    unsigned mapping_start_attempts = 0;
    std::chrono::steady_clock::time_point mapping_start_retry{};
    std::optional<ContentJob::Request> pending_content;
    std::unique_ptr<graphics::Canvas> prepared_image;
    std::vector<std::size_t> prepared_image_failures;
    std::uint64_t foreground_generation = 0;
    PP_DIAGNOSTIC(std::uint64_t diagnostic_session = 0, diagnostic_layout = 0;)
    bool content_loading = false, image_prepared = false;
    bool inline_image_pending = false, body_images_pending = false;
    std::chrono::steady_clock::time_point inline_image_after = std::chrono::steady_clock::time_point::max();
    std::string prepared_image_error;
    void abandon_content() {
        cancel_covers();
        ++foreground_generation;
        if (foreground_job) foreground_job->cancel();
        pending_content.reset();
        content_loading = image_prepared = false;
        inline_image_pending = false;
        prepared_image.reset();
        prepared_image_failures.clear();
    }
    Hit loading_cancel{};
    /**
     * Enters the loading state and presents its status with a cancel control. An open panel or
     * error page is replaced first, so the status never covers other controls; a full page keeps
     * its content and shows the status in the footer zone.
     */
    void loading(const std::string &status) {
        const bool panel = view == View::Tools || view == View::Contents || view == View::Typography ||
                           view == View::Refresh || view == View::Sync || view == View::Confirmation;
        if (panel && body_canvas) canvas = *body_canvas;
        else if (panel || view == View::Error) canvas.fill_rect(0, 104, 684, 1112, 15);
        const int top = panel || view == View::Reading || view == View::Error ? footer_top : footer_zone;
        content_loading = true;
        view = View::Loading;
        hits.clear();
        canvas.fill_rect(0, top, 684, 1216 - top, 15);
        if (top == footer_zone) canvas.line(0, top, 683, top, 1, 2);
        text(status, 36, 1156, 24, 400, 3);
        button(Dismiss, "取消", 488, 1140, 160, 64, false, 24);
        loading_cancel = hits.back();
        overlay();
    }
    void close_image_viewer() {
        viewer_original.reset();
        image_viewing = false; image_contrast_level = 0;
        view = View::Reading;
        // A bookmark committed while viewing changes the ribbon of the returned page.
        if (body_canvas && body_bookmarked != bookmarked(page.start, page.end) && render_body(offset)) { overlay(); return; }
        if (body_canvas) canvas = *body_canvas;
        links = body_links;
        overlay();
    }
    void draw_image_viewer(bool full = false) {
        if (!viewer_original) return;
        canvas = *viewer_original;
        if (image_contrast_level) {
            // Each curve preserves black and white while progressively separating middle tones.
            static constexpr std::uint8_t tones[viewer_contrast_levels][16] = {
                {0, 0, 1, 2, 3, 4, 5, 7, 8, 10, 11, 12, 13, 14, 15, 15},
                {0, 0, 0, 1, 2, 3, 5, 7, 8, 10, 12, 13, 14, 15, 15, 15},
                {0, 0, 0, 0, 1, 2, 4, 6, 9, 11, 13, 14, 15, 15, 15, 15},
            };
            const auto* curve = tones[image_contrast_level - 1];
            for (int y = viewer_top; y < viewer_top + viewer_height; ++y) {
                auto* row = canvas.data() + y * canvas.stride();
                for (int x = viewer_left / 2; x < (viewer_left + viewer_width) / 2; ++x) {
                    const auto packed = row[x];
                    row[x] = curve[packed & 15] | (curve[packed >> 4] << 4);
                }
            }
        }
        canvas.fill_rect(0, 0, 684, viewer_toolbar_height, 15);
        text("‹ 返回正文", 28, 30, 28, 300);
        canvas.line(viewer_action_left - 1, 0, viewer_action_left - 1, viewer_toolbar_height - 1, 1, 2);
        center("对比度 " + std::to_string(image_contrast_level) + "/3", viewer_action_left, 30,
               684 - viewer_action_left, 24);
        canvas.line(0, viewer_toolbar_height - 1, 683, viewer_toolbar_height - 1, 1, 2);
        image_viewing = true;
        view = View::Reading;
        overlay(full);
    }
    const graphics::ImageBox* image_at(int x, int y) const {
        if (view != View::Reading || image_viewing || content_loading || display.busy() ||
            pending || !card_available || usb_prepared || y >= footer_top)
            return nullptr;
        for (const auto& link : links)
            if (link.contains(x, y)) return nullptr;
        for (const auto& box : page.images)
            if (!box.resource.empty() && x >= box.x && x < box.x + box.width &&
                y >= box.y && y < box.y + box.height)
                return &box;
        return nullptr;
    }
    /**
     * Releases glyph cache bytes until PSRAM has the room a cold image decode requires before its
     * encoded bytes. The fonts belong to this task, so the worker cannot release them itself; trimmed
     * glyphs are rasterized again on demand. A disk-cache hit needs less and may be trimmed for nothing.
     */
    void make_image_room() {
#ifdef ESP_PLATFORM
        constexpr std::size_t wanted = ContentJob::image_room + 256U * 1024;
        for (auto* font : {&body, &ui}) {
            const auto available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            if (available >= wanted) return;
            const auto cached = font->cached_glyph_bytes();
            font->trim_glyph_cache(cached - std::min(cached, wanted - available), font->cached_glyphs());
            if (font->cached_glyph_bytes() < cached) ++cache_evictions;
        }
#endif
    }
    void begin_image_viewer(const graphics::ImageBox& box) {
        if (!body_canvas && !body_cache_room()) {
            message = "内存不足，无法查看图片";
            footer(); overlay();
            return;
        }
        // The cached canvas preserves the displayed page and its position while the worker decodes the image.
        body_canvas = canvas;
        body_spec = layout(settings, link_return ? 144 : 64);
        body_links = links;
        body_bookmarked = bookmarked(page.start, page.end);
        abandon_content();
        release_counting(); clear_prefetch(); make_image_room();
        ContentJob::Request request;
        request.kind = ContentJob::Kind::ImageViewer;
        request.root = root; request.path = root + "/" + book_path; request.identity = document.metadata();
        request.page.images.push_back({box.begin, viewer_left, viewer_top, viewer_width, viewer_height, box.resource});
        request.image_resource = box.resource;
        request.generation = foreground_generation;
        request.chapter = chapter_index; request.part = chapter.part;
        pending_content = std::move(request);
        loading("正在查看图片…");
    }
    bool begin_image(std::size_t start, const graphics::Page& planned, bool preview = false, bool progressive = false) {
        abandon_content();
        yield_counting_session();
        release_counting();
        ContentJob::Request request;
        request.kind = ContentJob::Kind::Image;
        PP_DIAGNOSTIC(
            request.diagnostic_session = diagnostic_session;
            request.diagnostic_layout = diagnostic_layout;
        )
        request.root = root; request.path = root + "/" + book_path; request.identity = document.metadata();
        request.preview = preview;
        request.progressive = progressive;
        request.layout = preview ? layout(draft, 64, preview_bottom) : layout(settings, link_return ? 144 : 64);
        request.page = planned;
        request.image_resource = planned.images.front().resource;
        if (planned.images.size() == 1 && planned.glyphs.empty())
            request.cache_name = image_cache_key(request.identity.book_id, request.image_resource, request.layout);
        request.generation = foreground_generation; request.chapter = chapter_index;
        request.part = chapter.part; request.offset = start;
        pending_content = std::move(request);
        clear_prefetch(); covers.clear(); make_image_room();
        if (progressive) { inline_image_pending = true; return true; }
        // The typography panel draws the preview status itself.
        if (preview) { content_loading = true; view = View::Loading; return true; }
        loading("正在准备图片…");
        clear_body_cache();
        return true;
    }
    bool begin_open(const std::string& path, ContentJob::Kind kind = ContentJob::Kind::Open) {
        cancel_sync();
        ContentJob::Request request;
        request.kind = kind;
        request.root = root; request.relative_path = path; request.path = root + "/" + path;
        request.generation = foreground_generation;
        struct stat info{};
        const auto found = identities.find(path);
        if (controlled_storage && found != identities.end() && stat(request.path.c_str(), &info) == 0 &&
            found->second.size == info.st_size && found->second.modified == info.st_mtime && found->second.inode == info.st_ino)
            request.identity = found->second.identity;
        request.content_revision = revision_for(path, request.identity);
        if (kind == ContentJob::Kind::Inspect && stat(request.path.c_str(), &info) == 0) {
            const auto cover = cover_entry(path, info, 120, 160, false);
            request.cache_name = cover.name; request.cache_header = cover.header;
            request.layout.width = 120; request.layout.height = 160;
        }
        pending_content = std::move(request);
        clear_prefetch(); covers.clear();
        loading("正在准备书籍…");
        if (kind == ContentJob::Kind::Open) { clear_body_cache(); release(chapter); page = {}; }
        return true;
    }
    bool begin_chapter(std::size_t index, std::uint32_t part, ContentJob::Target target,
                       std::size_t position = 0, const Anchor& anchor = {}, const std::string& fragment = "") {
        abandon_content();
        invalidate_counting();
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
        pending_content = std::move(request);
        loading("正在准备章节…");
        clear_body_cache(); clear_prefetch(); covers.clear(); release(chapter); page = {};
        return true;
    }
    bool begin_seek(const Anchor& anchor) {
        std::size_t index;
        if (!document.chapter_index_for(anchor.resource, index)) { error("目标位置不在正文中"); return false; }
        return begin_chapter(index, 0, ContentJob::Target::Anchor, 0, anchor);
    }
    /** Applies only image pixels; text, footer, anchor and reading revision stay owned by the visible page. */
    void complete_inline_image(const ContentJob& job) {
        const StageTimer timer{"image_patch", 0};
        const auto& request = job.request;
        inline_image_pending = false;
        if (view != View::Reading || image_viewing || request.generation != foreground_generation ||
            request.identity.book_id != document.metadata().book_id || request.chapter != chapter_index ||
            request.part != chapter.part || request.offset != page.start ||
            !same_layout(request.layout, layout(settings, link_return ? 144 : 64))) return;
        body_images_pending = false;
        PP_DIAGNOSTIC(const auto patch = diagnostics::begin(diagnostics::Kind::ImagePatch,
            {request.diagnostic_session, request.diagnostic_layout, ui_sequence + 1,
             request.generation, job.result.decoded_images});)
#ifdef ESP_PLATFORM
        if (!job.result.error.empty())
            ESP_LOGW("reader_app", "Inline image unavailable: error=%s psram_free=%u largest=%u",
                     job.result.error.c_str(), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#endif
        RefreshRegion region{684, 1216, 0, 0};
        int right = 0, bottom = 0;
        for (const auto& box : request.page.images) {
            const bool failed = !job.result.image || !job.result.error.empty() ||
                std::find(job.result.failed_images.begin(), job.result.failed_images.end(), box.begin) != job.result.failed_images.end();
            if (failed) {
                canvas.fill_rect(box.x, box.y, box.width, box.height, 15);
                draw_image_error(box);
            } else {
                // Preserve neighboring nibbles at odd edges; the interior is a packed row copy.
                const int left = (box.x + 1) & ~1;
                const int end = box.x + box.width;
                const int last = end & ~1;
                for (int y = box.y; y < box.y + box.height; ++y) {
                    if (box.x & 1) canvas.pixel(box.x, y, job.result.image->pixel(box.x, y));
                    if (last > left) std::memcpy(canvas.data() + y * canvas.stride() + left / 2,
                        job.result.image->data() + y * job.result.image->stride() + left / 2, (last - left) / 2);
                    if (end & 1) canvas.pixel(end - 1, y, job.result.image->pixel(end - 1, y));
                }
            }
            region.x = std::min(region.x, box.x); region.y = std::min(region.y, box.y);
            right = std::max(right, box.x + box.width); bottom = std::max(bottom, box.y + box.height);
        }
        if (body_canvas) *body_canvas = canvas;
        region.width = right - region.x; region.height = bottom - region.y;
        bool submitted = false;
        if (region.width > 0 && region.height > 0)
            submitted = display.present({{"ui", 0, 0, ++ui_sequence}, {}, FramePurpose::Overlay, RefreshIntent::Content, region});
        PP_DIAGNOSTIC(diagnostics::end(patch,
            submitted && job.result.image && job.result.error.empty() && job.result.failed_images.empty()
                ? diagnostics::Outcome::Success : diagnostics::Outcome::Failure);)
        (void)submitted;
    }
    void poll_content(bool allow_inline = true) {
        if (inline_image_pending && (view != View::Reading || image_viewing)) abandon_content();
        if (foreground_job && ((!content_loading && !inline_image_pending) || foreground_job->request.generation != foreground_generation)) {
            if (foreground_job->started() && !foreground_job->done()) return;
            foreground_job.reset();
        }
        if (!foreground_job && pending_content) {
            foreground_job = std::make_unique<ContentJob>(std::move(*pending_content));
            pending_content.reset();
            mapping_start_attempts = 0;
            mapping_start_retry = {};
        }
        if (!foreground_job) return;
        if (foreground_job->request.progressive && !allow_inline) return;
        if (!foreground_job->started()) {
            if (!content_loading && !inline_image_pending) { foreground_job.reset(); return; }
            if (display.busy() || counting_job || save_job || cover_job || bookmark_job) return;
            if (foreground_job->request.kind == ContentJob::Kind::Open && session.needs_save()) {
                if (save_failed) { foreground_job.reset(); content_loading = false; error(kSaveFailed); }
                else schedule_save();
                return;
            }
            const bool mapping = foreground_job->request.kind == ContentJob::Kind::MapDownload ||
                                 foreground_job->request.kind == ContentJob::Kind::MapUpload;
            if (mapping && std::chrono::steady_clock::now() < mapping_start_retry) return;
            if (!foreground_job->start()) {
                if (mapping) {
                    // Initialization or a full worker queue may recover on a later poll.
                    // Retries remain bounded and do not block input or repeat HTTP requests.
                    if (++mapping_start_attempts < 3) {
                        mapping_start_retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                        return;
                    }
                    const bool memory = foreground_job->start_failure() == ContentJob::StartFailure::InternalMemory;
                    foreground_job.reset(); content_loading = false;
                    cancel_sync();
                    sync_status = memory ? "内部内存不足，无法启动位置准备。请重新检查，本机位置未改动。"
                                         : "位置准备任务创建失败。请重新检查，本机位置未改动。";
                    draw_sync(); overlay(); return;
                }
                if (foreground_job->request.progressive) {
                    foreground_job->result.error = "Image worker unavailable";
                    complete_inline_image(*foreground_job); foreground_job.reset(); return;
                }
                foreground_job.reset(); content_loading = false;
                error("内容任务无法启动，请返回后重试。");
            }
            return;
        }
        if (!foreground_job->done() || display.busy()) return;
        auto job = std::move(foreground_job);
        const auto& request = job->request;
        if ((!content_loading && !inline_image_pending) || request.generation != foreground_generation) return;
        if (request.progressive) { complete_inline_image(*job); return; }
        if (request.kind == ContentJob::Kind::LockScreenList) {
            content_loading = false;
            lock_screens = std::move(job->result.lock_screens);
            lock_screen_error = job->result.error;
            draw_lock_screens(); overlay(); return;
        }
        if (request.kind == ContentJob::Kind::ImageViewer) {
            content_loading = false;
            if (request.chapter != chapter_index || request.part != chapter.part || !body_canvas) return;
            if (!job->result.error.empty() || !job->result.failed_images.empty() || !job->result.image) {
                canvas = *body_canvas;
                view = View::Reading;
                message = "图片无法放大";
                footer(); overlay();
                return;
            }
            viewer_original = std::move(job->result.image);
            image_contrast_level = 0;
            draw_image_viewer();
            return;
        }
        if (request.kind == ContentJob::Kind::MapUpload || request.kind == ContentJob::Kind::MapDownload) {
            content_loading = false;
            sync::Config current; std::string detail;
            if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
                cancel_sync(); sync_state = SyncState::ConfigChanged;
            } else if (request.kind == ContentJob::Kind::MapDownload) {
                accept_remote_mapping(job->result.mapped_anchor, job->result.mapped_place, job->result.error);
            } else if (!job->result.error.empty()) {
                upload_mapping_failed(job->result.error);
            } else if (!sync_checked || !request.record || !session.saved_record() || !(*session.saved_record() == *request.record)) {
                cancel_sync();
                sync_status = "阅读位置已经改变，请重新检查。";
            } else { send_upload(*request.record, job->result.xpointer, job->result.percentage); return; }
            draw_sync(); overlay(); return;
        }
        if (request.kind == ContentJob::Kind::Open || request.kind == ContentJob::Kind::Inspect) {
            content_loading = false;
            if (!job->result.error.empty()) {
                if (request.kind == ContentJob::Kind::Inspect) {
                    detail_error = job->result.error; draw_details(); overlay();
                } else error(reason(job->result.error, "无法解析文件内容") + "。原书没有被修改。", request.relative_path);
                return;
            }
            const auto& loaded = *job->result.document;
            if (controlled_storage) {
                if (identities.size() >= 8 && !identities.count(request.relative_path)) identities.erase(identities.begin());
                identities[request.relative_path] = {job->result.file_size, job->result.file_modified,
                    job->result.file_inode, loaded.metadata()};
            }
            if (request.kind == ContentJob::Kind::Inspect) {
                detail_metadata = loaded.metadata(); detail_progress = job->result.progress;
                detail_size = job->result.file_size; detail_cover = std::move(job->result.image);
                if (detail_progress.record) {
                    for (const auto& entry : loaded.toc())
                        if (entry.resource == detail_progress.record->anchor.resource) { detail_position = entry.label; break; }
                    if (detail_position.empty()) detail_position = "已保存阅读位置";
                }
                draw_details(); overlay(); return;
            }
            document = std::move(*job->result.document);
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
        prepared_image_failures = std::move(job->result.failed_images);
        prepared_image_error = job->result.error;
#ifdef ESP_PLATFORM
        if (!prepared_image_error.empty())
            ESP_LOGW("reader_app", "Image unavailable: resource=%s error=%s", request.image_resource.c_str(), prepared_image_error.c_str());
#endif
        if (request.preview) { draw_typography(); overlay(); }
        else show_page(request.offset);
        image_prepared = false;
        prepared_image.reset();
        prepared_image_failures.clear();
    }

    void stop_counting_job() {
        sleep_cover_deadline.reset();
        if (counting_job) { counting_job->cancel(); counting_job.reset(); }
        counting_session.reset();
        font_scan.reset();
    }
    void poll_counting_job() {
        if (!counting_job || !counting_job->done()) return;
        auto job = std::move(counting_job);
        const auto& request = job->request;
        const auto& result = job->result;
        if (request.generation != counting_generation) return;
        // The lock screen reads the cover cache itself; a failed or cancelled attempt falls back to the title.
        if (request.kind == ContentJob::Kind::SleepCover) return;
        if (request.kind == ContentJob::Kind::LockScreen) {
            if (settings.sleep_screen != "custom" || request.sleep_image != settings.sleep_image) return;
            if (request.preview && custom_sleep_attempted && sleep_cover_deadline) {
                custom_sleep_image = std::move(job->result.image);
                custom_notice = result.error;
                if (!custom_sleep_image && custom_notice.empty()) custom_notice = "休眠图片尚未就绪";
            }
            if (request.lock_screen_prepare) {
                custom_prepare_finished = result.cache_finished || !result.retryable_failure;
                custom_retry_at = result.cache_finished ? std::chrono::steady_clock::time_point{} :
                    std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }
            return;
        }
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
            if (request.font_scan != font_scan) return;
            if (!result.error.empty()) { pages_cache_disabled = true; font_scan.reset(); }
            else if (!result.font_identity.empty()) {
                font_identity = result.font_identity;
                font_scan.reset();
                load_pages_cache();
            }
            return;
        }
        if (request.chapter != counting_index ||
            request.part != counting_part || request.offset != counting_offset) return;
        if (!result.error.empty()) {
            if (memory_error(result.error)) defer_counting();
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
    std::size_t counting_blocked = not_counting;
    std::chrono::steady_clock::time_point counting_retry{};
    void defer_counting() {
        counting_blocked = chapter_index;
        counting_retry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
    bool counting_loaded = false;
    /** Set while a sleep request waits for its cover worker; holds the time to request cancellation. */
    std::optional<std::chrono::steady_clock::time_point> sleep_cover_deadline;
    bool sleep_waiting_for_cover = false;
    /** Starts the cover worker for a sleep request. The caller has stopped pagination and released prefetch memory. */
    void prepare_sleep_cover() {
        if (counting_job) return;
        struct stat info{};
        if (stat((root + "/" + book_path).c_str(), &info) != 0) return;
        const auto cover = cover_entry(book_path, info, 684, 1040, true);
        ContentJob::Request request;
        request.kind = ContentJob::Kind::SleepCover; request.root = root; request.path = root + "/" + book_path;
        request.cache_name = cover.name; request.cache_header = cover.header; request.cover_crop = true;
        request.layout.width = 684; request.layout.height = 1040; request.generation = counting_generation;
        counting_job = std::make_unique<ContentJob>(std::move(request));
        if (!counting_job->start()) counting_job.reset();
    }
    bool pages_complete = false, pages_failed = false, pages_dirty = false, pages_cache_pending = false;

    Impl(std::string card, std::string id)
        : root(std::move(card)), writer(std::move(id)), store(root) {}
    bool start(const Frame &frame) override {
        PP_DIAGNOSTIC(
            if (frame.purpose == FramePurpose::Body) {
                diagnostic_session = frame.token.session;
                diagnostic_layout = frame.token.layout;
            }
        )
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
            if (counting_job || cover_job || (bookmark_job && bookmark_job->started()) ||
                (foreground_job && foreground_job->started() && !foreground_job->done())) return;
            if (save_job->start()) return;
            save_job->result.progress.status = StoreStatus::IoError;
        } else if (!save_job->done()) return;
        auto job = std::move(save_job);
        const auto request = std::exchange(saving_request, std::nullopt);
        const bool okay = job->result.progress.status == StoreStatus::Ok &&
            job->result.progress.record && *job->result.progress.record == *job->request.record;
        if (request) session.saved(*request, okay);
        if (pending_position) {
            const auto &saved = *job->request.record;
            if (okay && saved.book_id == pending_position->book_id && saved.anchor == pending_position->anchor)
                store.save_cache(pending_position->name, pending_position->bytes);
            pending_position.reset();
        }
        save_failed = !okay;
        save_error_pending = !okay;
        if (!okay) message = kSaveFailed;
        if (job->result.last_book_saved) last_recorded = job->request.relative_path;
        else if (okay && job->request.save_last_book) {
            recent_save_failed = true; message = "最近阅读记录未保存";
        }
    }
    void drain_save() {
        drain_bookmark();
        if (!save_job) return;
        stop_covers(); clear_prefetch(); stop_counting_job();
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
        if (!saving_request && (!session.visible_record() || last_recorded == book_path || recent_save_failed)) return;
        ContentJob::Request request;
        request.kind = ContentJob::Kind::SaveProgress; request.root = root; request.relative_path = book_path;
        request.record = saving_request ? saving_request->record : *session.visible_record(); request.save_last_book = !recent_save_failed && last_recorded != book_path;
        save_job = std::make_unique<ContentJob>(std::move(request));
        if (counting_job) counting_job->cancel();
        poll_save();
    }
    /**
     * Commits the presented position after draining background commits. false when the position,
     * or a bookmark change drained here, was not stored; message then names what failed.
     */
    bool persist() {
        drain_save();
        save_error_pending = false;
        const bool bookmark_stored = !std::exchange(bookmark_drain_failed, false);
        if (!bookmark_stored) message = bookmark_drain_message;
        auto request = session.request_save();
        if (!request)
            return !session.needs_save() && bookmark_stored;
        const StageTimer timer{"progress_commit"};
        const auto result = store.save_progress(book_path, request->record);
        const bool okay =
            result.status == StoreStatus::Ok && result.record && *result.record == request->record;
        session.saved(*request, okay);
        save_failed = !okay;
        if (!okay)
            message = kSaveFailed;
        return okay && bookmark_stored;
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
    /** ASCII letters, digits and path punctuation; a run of these is not divided between lines. */
    static bool word_scalar(char32_t c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               c == '.' || c == '_' || c == '-' || c == '/' || c == '%';
    }
    /** Width of the undivided run that starts at index; 0 inside a run or outside one. */
    int run_width(const std::u32string &scalars, std::size_t index) {
        if (!word_scalar(scalars[index]) || (index && word_scalar(scalars[index - 1]))) return 0;
        int width = 0;
        for (; index < scalars.size() && word_scalar(scalars[index]); ++index) width += ui.advance(scalars[index]);
        return width;
    }
    /**
     * Draws a paragraph and returns its height. Closing punctuation never starts a line; it
     * extends past width by at most one character, which the page margin absorbs.
     */
    int wrapped(const std::string &value, int x, int top, int width, int size = 26,
                bool draw = true, std::uint8_t gray = 1) {
        ui.size(size);
        const auto metrics = ui.metrics();
        const int line = metrics.ascender + metrics.descender + 2;
        const auto scalars = decode_utf8(value);
        int used = 0, rows = 1;
        for (std::size_t i = 0; i < scalars.size(); ++i) {
            const int advance = ui.advance(scalars[i]), run = run_width(scalars, i);
            const bool closing = std::u32string_view(U"，。、；：！？）」』”’》").find(scalars[i]) != std::u32string_view::npos;
            if (used && !closing && (used + advance > width || (run <= width && used + run > width))) {
                used = 0;
                ++rows;
            }
            // A space that falls at the start of a continuation line is dropped.
            if (!used && rows > 1 && scalars[i] == U' ') continue;
            if (draw)
                ui.draw(canvas, scalars[i], x + used, top + (rows - 1) * line + metrics.ascender, gray);
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
     * the row a control; the chevron marks a control that opens another page.
     */
    void value_row(int id, const std::string &title, const std::string &description,
                   const std::string &value, int y, int height = 104, bool bold_value = false, bool opens = true) {
        const auto shown = !id || !opens ? value : value.empty() ? "›" : value + " ›";
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
    void outline(int x, int y, int w, int h, int weight = 2, std::uint8_t gray = 1) {
        canvas.fill_rect(x, y, w, weight, gray);
        canvas.fill_rect(x, y + h - weight, w, weight, gray);
        canvas.fill_rect(x, y, weight, h, gray);
        canvas.fill_rect(x + w - weight, y, weight, h, gray);
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
    /**
     * Page header: a back cell naming the parent page, when back is non-zero, directly followed
     * by the title. right_edge leaves room for action cells drawn by the caller.
     */
    void header(const std::string &title, int back = Back, const std::string &parent = "书库", int right_edge = 660) {
        canvas.fill_rect(0, 0, 684, 104, 15);
        int left = 28;
        if (back) {
            const auto label = "‹ " + parent;
            const int width = std::min(text_width(label, 28), 240);
            text(label, 28, 32, 28, width);
            left = 28 + width + 28;
            hits.push_back({0, 0, left, 104, back});
        }
        text(title, left, 30, 32, right_edge - left, 1, true);
        canvas.line(0, 103, 683, 103, 1, 3);
    }
    void disabled_button(const std::string &label, int x, int y, int w, int h = 88) {
        outline(x, y, w, h, 2, 5);
        center(label, x, y + (h - 26) / 2 - 3, w, 26, 5);
    }
    /** An action that cannot be undone: solid frame, never the filled recommended style. */
    void danger_button(int id, const std::string &label, int x, int y, int w, int h = 96) {
        outline(x, y, w, h);
        center(label, x, y + (h - 28) / 2 - 3, w, 28);
        hits.push_back({x, y, w, h, id});
    }
    /** The page notice; drawing it marks it for removal by the next input. */
    const std::string &notice() {
        notice_shown = notice_shown || !message.empty();
        return message;
    }
    static constexpr int footer_zone = 1120;
    /**
     * The 96-pixel footer shared by the screen pager and the page notice. With several screens
     * the notice takes the place of the screen number; a single screen shows the notice alone.
     * The pager zones match the reading page: previous left of 205, next from 479. A direction
     * that is unavailable stays blank and has no hit.
     */
    void footer_bar(const std::string &hint, std::size_t index = 0, std::size_t total = 1) {
        canvas.fill_rect(0, footer_zone, 684, 96, 15);
        if (total <= 1 && hint.empty()) return;
        canvas.line(0, footer_zone, 683, footer_zone, 1, 2);
        if (total <= 1) {
            text(hint, 36, footer_zone + 34, 24, 612);
            return;
        }
        for (int x : {205, 479}) canvas.line(x, footer_zone, x, 1215, 1, 2);
        if (index) {
            text("‹ 上一屏", 36, footer_zone + 34, 24, 160);
            hits.push_back({0, footer_zone, 205, 96, Prev});
        }
        if (index + 1 < total) {
            text("下一屏 ›", 648 - text_width("下一屏 ›", 24), footer_zone + 34, 24, 160);
            hits.push_back({479, footer_zone, 205, 96, Next});
        }
        if (hint.empty()) center(std::to_string(index + 1) + " / " + std::to_string(total), 205, footer_zone + 34, 274, 24, 3);
        else center(hint, 213, footer_zone + 36, 258, 22);
    }
    void title_lines(const std::string &value, int x, int y, int width, int size, bool bold = false, int max_rows = 2,
                     std::uint8_t gray = 1) {
        ui.size(size);
        const auto scalars = decode_utf8(value);
        int used = 0, row = 0;
        const int baseline = y + ui.metrics().ascender;
        for (std::size_t i = 0; i < scalars.size(); ++i) {
            const int advance = ui.advance(scalars[i]), run = run_width(scalars, i);
            if (used + advance > width || (used && row + 1 < max_rows && run <= width && used + run > width)) { ++row; used = 0; }
            if (row == max_rows - 1 && i + 1 < scalars.size() && used + advance + ui.advance(U'…') > width) {
                ui.draw(canvas, U'…', x + used, baseline + row * (size + 8), gray, bold);
                break;
            }
            if (row >= max_rows) break;
            ui.draw(canvas, scalars[i], x + used, baseline + row * (size + 8), gray, bold);
            used += advance;
        }
    }
    bool typography_changed() const {
        const auto &applied = edit_snapshot.config.settings;
        return reset_overrides || draft.font != applied.font || draft.font_weight != applied.font_weight || draft.font_size_px != applied.font_size_px ||
            draft.line_height != applied.line_height || draft.margin_x_px != applied.margin_x_px ||
            draft.paragraph_gap_lines != applied.paragraph_gap_lines || draft.indent_chars != applied.indent_chars;
    }
    std::string chapter_label() const {
        for (const auto &item : document.toc())
            if (item.resource == chapter.resource) return reading_footer::chapter_label(item.label, chapter_index);
        return reading_footer::chapter_label({}, chapter_index);
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
        return counted_page(chapter_index, chapter.part, start);
    }
    /** As current_page, for a scalar offset in any part of any spine item. */
    std::uint32_t counted_page(std::size_t index, std::uint32_t in_part, std::size_t start) const {
        const auto &spine = document.spine();
        if (!pages_complete || index >= spine.size() || !spine[index].linear) return 0;
        const auto &parts = chapter_pages[index];
        if (in_part >= parts.size() || parts[in_part].empty()) return 0;
        std::size_t before = 0;
        for (std::size_t i = 0; i < index; ++i)
            for (const auto &starts : chapter_pages[i]) before += starts.size();
        for (std::uint32_t part = 0; part < in_part; ++part) before += parts[part].size();
        const auto &starts = parts[in_part];
        const auto within = std::upper_bound(starts.begin(), starts.end(), static_cast<std::uint32_t>(start)) - starts.begin();
        return static_cast<std::uint32_t>(before + std::max<std::ptrdiff_t>(1, within));
    }
    /** Position weighted by the uncompressed size of each linear chapter; used until pages are counted. */
    std::pair<std::uint64_t, std::uint64_t> estimated_position(std::size_t end) const {
        return estimated_position(chapter_index, loaded_part(end));
    }
    /** The loaded chapter part with a scalar offset in it. */
    sync::PartPosition loaded_part(std::size_t offset) const {
        return {chapter.part, static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(chapter.text.size()),
                chapter.last_part};
    }
    /** As estimated_position(end), read up to at.offset in a part of any spine item. */
    std::pair<std::uint64_t, std::uint64_t> estimated_position(std::size_t index, const sync::PartPosition &at) const {
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
    double estimated_fraction(std::size_t end) const {
        const auto [read, total] = estimated_position(end);
        return total ? static_cast<double>(read) / total : 0;
    }
    /** Whole-book percentage of a page spanning [start, end) of the loaded chapter. */
    int book_percent_value(std::size_t start, std::size_t end) const {
        return book_percent_value(chapter_index, loaded_part(end), start);
    }
    /**
     * Whole-book percentage of a position in any spine item: the counted page that holds start
     * once every chapter is counted, otherwise the size read up to at.offset.
     */
    int book_percent_value(std::size_t index, const sync::PartPosition &at, std::size_t start) const {
        if (const auto current = counted_page(index, at.part, start))
            return static_cast<int>(static_cast<std::uint64_t>(current) * 100 / total_pages());
        const auto [read, total] = estimated_position(index, at);
        return total ? static_cast<int>(read * 100 / total) : 0;
    }
    std::string book_percent(std::size_t start, std::size_t end) const {
        return std::to_string(book_percent_value(start, end)) + "%";
    }
    std::string book_percent() const {
        return book_percent(page.start, page.end);
    }
    /** Page number, page total and percentage; the percentage alone until every chapter is counted. */
    std::string book_position(std::size_t start, std::size_t end) const {
        if (const auto current = current_page(start))
            return std::to_string(current) + " / " + std::to_string(total_pages()) + " · " + book_percent(start, end);
        if (pages_complete) return "共 " + std::to_string(total_pages()) + " 页 · " + book_percent(start, end);
        return book_percent(start, end);
    }
    std::string footer_position_text() const {
        switch (footer_position) {
        case reading_footer::Position::Chapters:
            return "第 " + std::to_string(chapter_index + 1) + " 章 / 共 " + std::to_string(document.spine().size()) + " 章";
        case reading_footer::Position::ChapterName:
            return chapter_label();
        case reading_footer::Position::Percent:
            if (const auto current = current_page())
                return reading_footer::percent(static_cast<double>(current) / total_pages());
            return reading_footer::percent(estimated_fraction(page.end), pages_complete);
        case reading_footer::Position::Pages:
            if (const auto current = current_page())
                return std::to_string(current) + " / " + std::to_string(total_pages());
            if (pages_complete) return "共 " + std::to_string(total_pages()) + " 页";
            if (pages_failed) return "页数未知";
            return counting_blocked == chapter_index ? "等待统计页数" : "正在统计页数";
        }
        return {};
    }
    std::string sync_badge() const {
        if (txt_name(book_path)) return "仅本地";
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
    /**
     * Book header. Over a tool panel the back cell closes the panel, because the panel's parent
     * is the reading page. The badge appears only for a state the user has to act on.
     */
    void reading_header(bool panel = false) {
        canvas.fill_rect(0, 0, 684, 104, 15);
        text(panel ? "‹ 正文" : "‹ 书库", 28, 32, 28, 100);
        hits.push_back({0, 0, 128, 104, panel ? Back : Library});
        const bool unsaved = session.needs_save(), differs = remote_anchor.has_value();
        // The tool page adds the bookmark cell at the right end and moves the badge left of it.
        const bool marking = view == View::Tools;
        const int edge = marking ? bookmark_cell_left - 12 : 660;
        const int right = unsaved || differs ? edge - 164 : edge;
        text(chapter_label(), 132, 14, 30, right - 132, 1, true);
        const auto percent = " · " + book_percent();
        const int percent_width = text_width(percent, 22);
        const int title_width = std::min(text_width(document.metadata().title, 22), right - 132 - percent_width);
        text(document.metadata().title, 132, 60, 22, title_width, 3);
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
    // Left edge of the bookmark toggle in the tool page header; the reading page marker shares its centre.
    static constexpr int bookmark_cell_left = 584;
    static constexpr int panel_strip = 56;
    // The typography preview ends above the strip of the panel at 656.
    static constexpr int preview_bottom = 656 - panel_strip;
    /**
     * Panel top: a strip that closes the panel, above the four tool tabs. The strip occupies
     * panel_strip pixels above top.
     */
    void tabs(int top, int active) {
        const int ids[] = {Contents, Typography, Sync, Refresh};
        const char *names[] = {"目录", "排版", "同步", "刷新"};
        canvas.fill_rect(0, top - panel_strip, 684, panel_strip + 100, 15);
        canvas.line(0, top - panel_strip, 683, top - panel_strip, 1, 3);
        text("收起", 574, top - panel_strip + 14, 24, 60, 3);
        canvas.line(634, top - 32, 641, top - 25, 3, 2);
        canvas.line(641, top - 25, 648, top - 32, 3, 2);
        hits.push_back({0, top - panel_strip, 684, panel_strip, Back});
        for (int i = 0; i < 4; ++i) {
            const int x = i * 171;
            if (ids[i] == active) canvas.fill_rect(x, top, 171, 100, 1);
            else if (i) canvas.line(x, top, x, top + 99, 1, 2);
            center(names[i], x, top + 32, 171, 26, ids[i] == active ? 15 : 1, ids[i] == active);
            hits.push_back({x, top, 171, 100, ids[i]});
        }
        canvas.line(0, top, 683, top, 1, 2);
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
        } else {
            // A screen painted solid: the whole panel is redrawn.
            outline(cx - 15, cy - 18, 30, 36, 3);
            canvas.fill_rect(cx - 9, cy - 12, 18, 24, 1);
        }
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
        } else footer_bar(status);
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
        const auto resolved = weight_font(selected);
        weight_unavailable = selected.font_weight == "light" && !light_available(selected);
        const auto tag = selected.font_weight == "bold" ? "-Bold." : selected.font_weight == "medium" ? "-Medium." : selected.font_weight == "light" ? "-Light." : "-Regular.";
        const bool native_weight = resolved.find(tag) != std::string::npos;
        const bool weighted_base = resolved.find("-Bold.") != std::string::npos || resolved.find("-Medium.") != std::string::npos || resolved.find("-Light.") != std::string::npos;
        weight_unavailable = weight_unavailable || (selected.font_weight != "regular" && !native_weight && weighted_base);
        body.weight(native_weight || weight_unavailable ? 0 : selected.font_weight == "bold" ? 2 : selected.font_weight == "medium" ? 1 : 0);
        if (effective_font == resolved && body.valid() && controlled_storage) {
            if (effective_weight != selected.font_weight) { clear_body_cache(); clear_prefetch(); }
            effective_weight = selected.font_weight;
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
    }
    /**
     * Lists the TTF and OTF files in Fonts/, reading at most 2048 directory entries and keeping
     * at most 256 fonts; a larger folder yields a partial list. The selected font stays listed
     * while its file is missing.
     */
    void scan_fonts() {
        fonts.clear();
        DIR *dir = opendir((root + "/Fonts").c_str());
        if (dir) {
            for (int visited = 0; visited < 2048 && fonts.size() < 256; ++visited) {
                const auto *item = readdir(dir);
                if (!item) break;
                const std::string name = item->d_name;
                if (name.size() <= 4 || name.front() == '.') continue;
                auto extension = name.substr(name.size() - 4);
                for (auto &c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (extension == ".ttf" || extension == ".otf") fonts.push_back("Fonts/" + name);
            }
            closedir(dir);
            std::sort(fonts.begin(), fonts.end());
        }
        if (std::find(fonts.begin(), fonts.end(), settings.font) == fonts.end())
            fonts.push_back(settings.font);
    }
    bool initialize() {
        refresh_badge_config();
        if (!ui.open(root + "/Fonts/NotoSansSC-Regular.otf") && !ui.open_fallback())
            return false;
        settings = store.load_settings().settings;
        library_grid = settings.library_view == "grid";
        library_sort = settings.library_sort;
        std::string detail;
        const auto recent = store.load_last_book(detail);
        if (recent) {
            last_book = *recent;
            last_recorded = *recent;
        }
        scan_fonts();
        load_fonts();
        scan_library();
        draw_library();
        update_boot();
        overlay(true);
        return true;
    }
    void scan_library() {
        entries.clear();
        folder_root = false;
        library_home_root = settings.library_roots.size() == 1 ? settings.library_roots.front() : "";
        library_directory_unavailable = false;
        if (!card_available)
            return;
        const auto cfg = store.load_settings();
        library_sort = cfg.settings.library_sort;
        folders_first = cfg.settings.library_folders == "first";
        const auto &roots = cfg.settings.library_roots;
        library_home_root = roots.size() == 1 ? roots.front() : "";
        folder_root = std::find(roots.begin(), roots.end(), folder) != roots.end();
        const auto listing = folder.empty() ? library_home_root : folder;
        if (listing.empty()) {
            for (const auto &path : roots) {
                struct stat st{};
                if (safe_existing(root, path) && stat((root + "/" + path).c_str(), &st) == 0 &&
                    S_ISDIR(st.st_mode))
                    entries.push_back({path, basename(path), true});
            }
        } else {
            DIR *dir = safe_existing(root, listing) ? opendir((root + "/" + listing).c_str()) : nullptr;
            if (!dir) {
                library_directory_unavailable = true;
                message = "无法读取目录，请检查 TF 卡";
                return;
            }
            while (const auto *item = readdir(dir)) {
                const std::string name = item->d_name;
                if (name.empty() || name[0] == '.' || name.size() > 240 ||
                    name.find(".picopage") != std::string::npos)
                    continue;
                const auto path = listing + "/" + name;
                struct stat st{};
#ifdef ESP_PLATFORM
                const int result = stat((root + "/" + path).c_str(), &st);
#else
                const int result = lstat((root + "/" + path).c_str(), &st);
#endif
                if (result != 0)
                    continue;
                if (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && book_name(name)))
                    entries.push_back({path, name, S_ISDIR(st.st_mode)});
                if (entries.size() >= 512) {
                    message = "当前目录仅显示前 512 项，请分目录存放";
                    break;
                }
            }
            closedir(dir);
        }
        std::string history_error;
        const auto recent = library_sort == "recent" ? store.load_recent_books(history_error) : std::vector<RecentBook>{};
        library_recent = library_sort == "recent" && history_error.empty();
        if (library_sort == "recent" && !history_error.empty()) message = "最近阅读记录无法读取，暂按书名排序";
        for (auto& entry : entries) {
            for (std::size_t i = 0; i < recent.size(); ++i) {
                if (recent[i].path == entry.path || (entry.directory && recent[i].path.compare(0, entry.path.size() + 1, entry.path + "/") == 0)) {
                    entry.recent_rank = i; entry.opened_at = recent[i].opened_at; break;
                }
            }

        }
        library_resume_omitted = false;
        const auto resume = has_book() ? book_path : last_book;
        resume_opened_at = 0;
        for (const auto& item : recent) if (item.path == resume) { resume_opened_at = item.opened_at; break; }
        if (library_recent && (folder.empty() || folder_root) && !resume.empty()) {
            const auto end = std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
                return !entry.directory && entry.path == resume;
            });
            library_resume_omitted = end != entries.end(); entries.erase(end, entries.end());
        }
        std::sort(entries.begin(), entries.end(), [this](const Entry &a, const Entry &b) {
            if ((folders_first || !library_recent) && a.directory != b.directory) return a.directory > b.directory;
            if (library_recent && a.recent_rank != b.recent_rank) return a.recent_rank < b.recent_rank;
            if (library_recent && a.directory != b.directory) return a.directory > b.directory;
            return library_sort == "name_desc" ? a.name > b.name : a.name < b.name;
        });
        library_page = std::min(library_page, library_pages() - 1);
    }
    bool library_resume() const { return (folder.empty() || folder_root) && (has_book() || !last_book.empty()); }
    std::size_t library_page_size() const { return library_grid ? 6 : library_resume() ? 5 : 7; }
    std::size_t library_begin() const { return library_page * library_page_size(); }
    std::size_t library_pages() const {
        const auto size = library_page_size();
        return entries.empty() ? 1 : 1 + (entries.size() - 1) / size;
    }
    std::string recent_date(std::int64_t utc) const {
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        if (now < provision::clock_floor || utc < provision::clock_floor || utc > now) return "";
        const auto days = (now + 8 * 3600) / 86400 - (utc + 8 * 3600) / 86400;
        if (days == 0) return "今天";
        if (days == 1) return "昨天";
        if (days < 30) return std::to_string(days) + " 天前";
        const auto shifted = static_cast<std::time_t>(utc + 8 * 3600);
        std::tm date{};
        if (!gmtime_r(&shifted, &date)) return "";
        char value[12]; std::strftime(value, sizeof(value), "%m-%d", &date); return value;
    }
    static std::string entry_title(const Entry &entry) {
        return entry.directory ? entry.name : entry.name.substr(0, entry.name.size() - (txt_name(entry.name) ? 4 : 5));
    }
    /** FNV-1a of a card-relative path; names the cache entries of one book. */
    static unsigned long long path_hash(const std::string &relative) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char c : relative) hash = (hash ^ c) * 1099511628211ULL;
        return hash;
    }
    struct PendingPosition {
        std::string name, bytes, book_id;
        Anchor anchor;
    };
    std::optional<PendingPosition> pending_position;
    static std::string position_name(const std::string &relative) {
        char name[40];
        std::snprintf(name, sizeof(name), "positions/%016llx", path_hash(relative));
        return name;
    }
    static std::string position_header(const std::string &relative, const struct stat &info) {
        return "PPPOS1\n" + relative + "\n" + std::to_string(info.st_size) + " " +
               std::to_string(static_cast<long long>(info.st_mtime)) + "\n";
    }
    /**
     * Writes the visible chapter and percentage to PicoPage/cache for the library. The entry is
     * display data bound to one file version; the progress record remains the only authority.
     */
    void remember_position() {
        struct stat info{};
        if (!has_book() || !card_available || !session.visible_record() || page.end <= page.start ||
            stat((root + "/" + book_path).c_str(), &info) != 0) return;
        const auto name = position_name(book_path);
        auto bytes = position_header(book_path, info) + chapter_label() + "\n" + book_percent();
        // The library must not show a position that reopening cannot restore: an unsaved page is
        // recorded by poll_save once its commit is acknowledged.
        if (session.needs_save()) {
            pending_position = PendingPosition{name, std::move(bytes), document.metadata().book_id, session.visible_record()->anchor};
            return;
        }
        pending_position.reset();
        store.save_cache(name, bytes);
    }
    /** Chapter and percentage last recorded for a book file; both empty when there is no entry. */
    std::pair<std::string, std::string> recalled_position(const std::string &relative) {
        struct stat info{};
        std::string cached;
        if (stat((root + "/" + relative).c_str(), &info) != 0) return {};
        const auto header = position_header(relative, info);
        if (store.load_cache(position_name(relative), header.size() + 512, cached) != StoreStatus::Ok ||
            cached.compare(0, header.size(), header) != 0) return {};
        const auto split = cached.find('\n', header.size());
        if (split == std::string::npos) return {};
        return {cached.substr(header.size(), split - header.size()), cached.substr(split + 1)};
    }
    /** Fills the status line of a library entry: books in a folder, or the reading state of a book. */
    void describe(Entry &entry) {
        if (entry.described) return;
        entry.described = true;
        if (entry.directory) {
            int books = 0;
            if (DIR *dir = opendir((root + "/" + entry.path).c_str())) {
                while (const auto *item = readdir(dir))
                    if (item->d_name[0] != '.' && book_name(item->d_name)) ++books;
                closedir(dir);
            }
            entry.status = "文件夹 · " + std::to_string(books) + " 本";
            return;
        }
        entry.status = recalled_position(entry.path).second;
        if (!entry.status.empty()) return;
        // Without a cache entry only the existence of a position is known. A cleared record is
        // a small file that names its state.
        char record[512] = {};
        std::size_t length = 0;
        bool unreadable = false;
        for (const auto* suffix : {"/progress.json", "/progress.json.bak"}) {
            if (std::FILE *file = std::fopen((root + "/" + entry.path + ".picopage" + suffix).c_str(), "rb")) {
                length = std::fread(record, 1, sizeof(record) - 1, file);
                unreadable = unreadable || std::ferror(file);
                std::fclose(file);
                if (length) break;
            } else if (errno != ENOENT) unreadable = true;
        }
        entry.status = unreadable ? "位置未知" : length && !std::strstr(record, "\"cleared\"") ? "在读" :
                       entry.recent_rank < 128 ? "已打开" : "未读";
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
        return {name, "PPCOVER2\n" + relative + "\n" + std::to_string(info.st_size) + " " +
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
    // Smaller library covers use the card thumbnail, so these views share one cache entry.
    static constexpr int kRowCoverWidth = 59, kRowCoverHeight = 80;
    static constexpr int kContinueCoverWidth = 96, kContinueCoverHeight = 128;
    /** Copies packed cover pixels reduced to width x height by averaging the covered source pixels. */
    void blit_reduced(const std::vector<std::uint8_t> &pixels, int source_width, int source_height,
                      int x, int y, int width, int height) {
        const int stride = (source_width + 1) / 2;
        for (int row = 0; row < height; ++row) {
            const int top = row * source_height / height, bottom = std::max(top + 1, (row + 1) * source_height / height);
            for (int column = 0; column < width; ++column) {
                const int left = column * source_width / width, right = std::max(left + 1, (column + 1) * source_width / width);
                int sum = 0;
                for (int v = top; v < bottom; ++v)
                    for (int u = left; u < right; ++u) {
                        const auto packed = pixels[v * stride + u / 2];
                        sum += u % 2 ? packed >> 4 : packed & 15;
                    }
                canvas.pixel(x + column, y + row, static_cast<std::uint8_t>(sum / ((bottom - top) * (right - left))));
            }
        }
    }
    enum class Cover { Drawn, None, Undecoded };
    /**
     * Draws a library thumbnail at the requested size. Without decode, a cover absent from both
     * caches is left Undecoded.
     */
    Cover draw_cover(const Entry &entry, int x, int y, bool decode,
                     int width = kLibraryCoverWidth, int height = kLibraryCoverHeight) {
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
            const auto cached = cover_entry(entry.path, info, kLibraryCoverWidth, kLibraryCoverHeight, true);
            std::vector<std::uint8_t> pixels;
            if (!cached_cover(cached, pixels)) {
                if (!decode || async_content) {
                    if (async_content && std::none_of(missing_covers.begin(), missing_covers.end(),
                        [&](const auto &source) { return source.path == entry.path; }))
                        missing_covers.push_back({entry.path, cached.name, cached.header, info.st_size, info.st_mtime});
                    return Cover::Undecoded;
                }
                pixels = decode_cover(entry.path, cached, kLibraryCoverWidth, kLibraryCoverHeight, true);
            }
            if (covers.size() == kCoverSlots) covers.erase(covers.begin());
            covers.push_back({entry.path, info.st_size, info.st_mtime, std::move(pixels)});
            found = covers.end() - 1;
        }
        // An entirely white thumbnail identifies nothing; the title is drawn in its place.
        if (std::all_of(found->pixels.begin(), found->pixels.end(), [](std::uint8_t packed) { return packed == 0xff; }))
            return Cover::None;
        if (width == kLibraryCoverWidth && height == kLibraryCoverHeight)
            blit(found->pixels, x, y, width, height);
        else blit_reduced(found->pixels, kLibraryCoverWidth, kLibraryCoverHeight, x, y, width, height);
        return Cover::Drawn;
    }
    /** The scanned directory as it reads after a verb, with the spacing of mixed Chinese and Latin text. */
    std::string library_location() const {
        return library_home_root.empty() ? "已设置的书库目录" : " " + library_home_root + "/";
    }
    /**
     * One line for the row that describes copying to the card. A directory too long for the row
     * keeps its last components, so the instruction after it is never the part that is cut.
     */
    std::string direct_copy_hint() {
        auto path = library_home_root;
        bool cut = false;
        const auto line = [&] { return "拷到卡上 " + std::string(cut ? "…/" : "") + path + "/，插回后重启设备"; };
        for (auto slash = path.find('/'); text_width(line(), 22) > 554 && slash != std::string::npos; slash = path.find('/')) {
            path.erase(0, slash + 1);
            cut = true;
        }
        return path.empty() || text_width(line(), 22) > 554 ? "拷到卡上已设置的书库目录，插回后重启设备" : line();
    }
    /**
     * Body of the root library without any book or resume card: the ways to add books, the
     * guide and the limit of the built-in font. The row without an id is a description.
     */
    void draw_first_use() {
        text("书库里还没有书", 36, 160, 40, 612, 1, true);
        text("选一种方式放入 EPUB 或 TXT", 36, 222, 26, 612, 3);
        canvas.line(24, 288, 660, 288, 1, 2);
        settings_row(UsbStart, "USB 连接电脑", "电脑直接管理 TF 卡，退出时重启", 288, 1);
        settings_row(WifiUpload, "Wi-Fi 传书", "经设备热点，手机扫码上传，不需要路由器", 420, 1);
        settings_row(0, "直接放入 TF 卡", direct_copy_hint(), 552, 1);
        text("第一次使用", 36, 760, 22, 612, 3);
        button(Help, "查看使用说明", 24, 796, 636, 96, false, 30);
        wrapped(std::string("内置字体只含 3,500 个常用字，生僻字会显示为方框。官网 ") + kProjectHome +
                " 可下载完整的文楷字体，放入 TF 卡 Fonts/，重启后在「排版」中选择。", 36, 924, 612, 22, true, 3);
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
        if (folder.empty()) header("书库", 0, "", 388);
        else {
            const auto slash = folder.find_last_of('/');
            const auto parent = slash == std::string::npos ? "" : folder.substr(0, slash);
            header(basename(folder), Back, folder_root || parent.empty() || parent == library_home_root ? "书库" : basename(parent), 388);
            hits.back() = {0, 0, 396, 104, Back};
        }
        for (int x : {396, 540}) canvas.line(x, 0, x, 102, 1, 2);
        center("导入", 396, 32, 144, 26);
        center("设置", 540, 32, 144, 26);
        hits.push_back({396, 0, 144, 104, Import});
        hits.push_back({540, 0, 144, 104, Settings});
        // The card appears where reading normally starts and only when there is a book to resume.
        int top = 104;
        if (library_resume()) {
            std::string position;
            if (has_book() && page.end > page.start) position = chapter_label() + " · " + book_percent();
            else if (!has_book()) {
                const auto recalled = recalled_position(last_book);
                if (!recalled.second.empty()) position = recalled.first + " · " + recalled.second;
            }
            outline(24, 128, 636, 206, 3);
            text("继续阅读", 48, 146, 22, 520, 3);
            const auto recent = has_book() ? book_path : last_book;
            const auto cover = draw_cover(Entry{recent, basename(recent), false}, 48, 184, decode,
                                          kContinueCoverWidth, kContinueCoverHeight);
            undecoded = undecoded || cover == Cover::Undecoded;
            if (cover == Cover::Drawn) outline(47, 183, kContinueCoverWidth + 2, kContinueCoverHeight + 2);
            const int left = cover == Cover::Drawn ? 168 : 48;
            const int width = cover == Cover::Drawn ? 420 : 540;
            title_lines(has_book() ? document.metadata().title : basename(last_book), left, 182, width, 30, true);
            const auto date = library_recent ? recent_date(resume_opened_at) : "";
            if (!date.empty()) position += (position.empty() ? "" : " · ") + date;
            text(position.empty() ? "打开后恢复已保存的位置" : position, left, 288, 22, width, 3);
            text("›", 620, 214, 28, 30);
            hits.push_back({24, 128, 636, 206, Tools});
            top = 354;
        }
        if (entries.empty() && !library_resume_omitted) {
            if (card_available && !library_directory_unavailable && folder.empty() && top == 104) {
                draw_first_use();
                footer_bar(notice());
                return;
            }
            const int y = top + 120;
            text(!card_available ? "存储卡不可用" : library_directory_unavailable ? "无法读取书库目录" :
                 folder.empty() ? "书库里还没有书" : "这个文件夹是空的", 36, y, 40, 612, 1, true);
            if (!card_available || library_directory_unavailable)
                wrapped(!card_available ? "这不代表书籍已被删除。" : "请检查书库目录和 TF 卡。", 36, y + 76, 612, 28);
            else {
                const int height = wrapped(folder.empty() ? "把 EPUB 或 TXT 放入" + library_location() + "，或使用“导入”。"
                                                          : "用“导入”把 EPUB 或 TXT 复制到存储卡，或返回上一级。", 36, y + 76, 612, 28);
                button(Import, "导入", 24, y + 104 + height, 636, 96);
            }
            footer_bar(notice());
            return;
        }
        const auto folders = static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(), [](const Entry &entry) { return entry.directory; }));
        std::string count = folders ? std::to_string(folders) + " 个文件夹" : "";
        if (folders < entries.size()) count += (count.empty() ? "" : " · ") + std::to_string(entries.size() - folders) + " 本书";
        if (folders && folders < entries.size()) {
            text((library_resume_omitted ? "另有 " : "") + std::to_string(entries.size() - folders) + " 本书", 36, top + 8, 22, 254, 3);
            text(std::to_string(folders) + " 个文件夹", 36, top + 38, 20, 254, 3);
        } else {
            if (library_resume_omitted) count = "另有 " + std::to_string(entries.size() - folders) + " 本书";
            text(count, 36, top + 24, 22, 254, 3);
        }
        button(LibrarySort, library_sort == "recent" ? (library_recent ? "最近阅读 ⇅" : "书名（回退）") : library_sort == "name_desc" ? "书名 ↓" : "书名 ↑", 300, top + 4, 164, 60, false, 22);
        button(LibraryList, "列表", 476, top + 4, 92, 60, !library_grid, 22);
        button(LibraryGrid, "卡片", 568, top + 4, 92, 60, library_grid, 22);
        canvas.line(0, top + 76, 683, top + 76, 1, 2);
        const auto size = library_page_size(), begin = library_begin();
        int group_gap = 0;
        for (std::size_t i = begin; i < std::min(begin + size, entries.size()); ++i) {
            auto &entry = entries[i];
            describe(entry);
            const auto label = entry_title(entry);
            if (library_grid) {
                const int x = 36 + static_cast<int>((i - begin) % 3) * 214;
                const int y = top + 100 + static_cast<int>((i - begin) / 3) * 320;
                if (entry.directory) {
                    // A tabbed frame distinguishes a folder from a book without a cover.
                    canvas.fill_rect(x, y, 84, 2, 1);
                    canvas.fill_rect(x, y, 2, 22, 1);
                    canvas.fill_rect(x + 82, y, 2, 22, 1);
                    outline(x, y + 22, kLibraryCoverWidth, kLibraryCoverHeight - 22);
                    title_lines(label, x + 18, y + 162, 148, 28, true);
                } else {
                    const auto cover = draw_cover(entry, x, y, decode);
                    undecoded = undecoded || cover == Cover::Undecoded;
                    if (cover != Cover::Drawn) {
                        title_lines(label, x + 18, y + 20, 148, 28, true, 4);
                        canvas.fill_rect(x + 18, y + 228, 48, 2, 1);
                    }
                    outline(x, y, kLibraryCoverWidth, kLibraryCoverHeight);
                }
                text(entry.status, x, y + 264, 22, kLibraryCoverWidth, 3);
                hits.push_back({x, y, kLibraryCoverWidth, 296, EntryBase + static_cast<int>(i)});
            } else {
                if (library_recent && !entry.directory && entry.recent_rank == 128 &&
                    (i == begin || entries[i - 1].directory || entries[i - 1].recent_rank < 128)) {
                    const int divider = top + 78 + static_cast<int>(i - begin) * 126;
                    text("其他书籍", 36, divider + 4, 20, 150, 5);
                    canvas.line(156, divider + 18, 648, divider + 18, 12);
                    group_gap = 32;
                }
                const int y = top + 78 + static_cast<int>(i - begin) * 126 + group_gap;
                // A book without a cover leaves no empty slot; its title starts at the margin.
                const auto cover = entry.directory ? Cover::None : draw_cover(entry, 37, y + 22, decode,
                                                                                kRowCoverWidth, kRowCoverHeight);
                undecoded = undecoded || cover == Cover::Undecoded;
                if (cover == Cover::Drawn) outline(36, y + 21, kRowCoverWidth + 2, kRowCoverHeight + 2);
                const int left = cover == Cover::Drawn ? 118 : 36;
                const auto date = library_recent ? recent_date(entry.opened_at) : "";
                const auto status = entry.directory ? entry.status + " ›" : entry.status;
                const int status_width = std::min(std::max(text_width(status, 22), text_width(date, 22)), 200);
                const int title_width = 628 - left - status_width;
                title_lines(label, left, y + (text_width(label, 28) <= title_width ? 44 : 26), title_width, 28);
                if (!date.empty()) {
                    text(date, 648 - text_width(date, 22), y + 30, 22, 200, 3);
                    text(status, 648 - status_width, y + 66, 20, status_width, 3);
                } else text(status, 648 - status_width, y + 50, 22, status_width, 3);
                canvas.line(24, y + 125, 660, y + 125, 12, 2);
                hits.push_back({24, y, 636, 126, EntryBase + static_cast<int>(i)});
            }
        }
        footer_bar(undecoded ? "正在生成封面…" : notice(), library_page, library_pages());
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
        invalidate_counting();
        if (counting_job) counting_job->cancel();
        // A remote position kept as a header reminder belongs to the book being left.
        remote_anchor.reset();
        refresh_badge_config();
        clear_body_cache();
        clear_prefetch();
        if (!safe_existing(root, path)) {
            error("书籍路径无效或包含符号链接");
            return false;
        }
        if (has_book()) {
            if (async_content) { remember_position(); schedule_save(); }
            else if (!persist()) { error(message); return false; }
            else remember_position();
        }
        save_pages();
        if (async_content) return begin_open(path);
        std::string detail;
        if (!open_document(path, detail)) {
            book_path.clear();
            session.open("", "", writer);
            error(reason(detail, "无法解析文件内容") + "。原书没有被修改。", path);
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
        last_recorded.clear();
        recent_save_failed = false;
        const auto configuration = loaded ? loaded->configuration : store.load_settings(path);
        settings = configuration.settings;
        load_fonts();
        clear_page_history();
        reset_pages();
        load_bookmarks(loaded ? &loaded->bookmarks : nullptr);
        contents_bookmarks = false;
        const auto restored =
            loaded ? loaded->progress : store.load_progress(path, document.metadata().book_id, revision_for(path, document.metadata()));
        session.open(document.metadata().book_id, revision_for(path, document.metadata()), writer, restored.record);
        if (restored.status != StoreStatus::Ok && restored.status != StoreStatus::Missing && restored.status != StoreStatus::Cleared) {
            error("原进度读取失败。为保护阅读记录，已停止打开本书。原书和进度文件都没有被修改。", path);
            return false;
        }
        chapter_index = 0;
        offset = 0;
        message.clear();
        if (!configuration.diagnostics.empty())
            message = "设置文件无效，已回退；原文件未修改";
        else if (!bookmarks_usable())
            message = bookmarks.status == StoreStatus::IdentityMismatch ? "书签与当前原书不符，未使用；原文件已保留"
                                                                        : "书签文件无法读取，已保留原文件；阅读进度不受影响";
        else if (font_fallback)
            message = builtin_body ? kBuiltinFontNotice : kFallbackFontNotice;
        else if (weight_unavailable)
            message = kWeightUnavailableNotice;
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
                error(memory_error(detail) ? chapter_error(detail) : "保存的位置无法映射，请保留进度文件并检查原书");
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
        return memory_error(detail) ? "本章内容过大，设备内存不足，无法显示。" : "章节读取失败。" + reason(detail, "无法解析文件内容") + "。";
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
        text(notice().empty() ? footer_position_text() : message, 36, 1156, 24, 476, 3);
        if (footer_device == reading_footer::Device::Clock) {
            // The system clock is restored from RTC or calibrated by on-demand networking.
            center(reading_footer::clock(std::time(nullptr), 8 * 60), reading_footer::device_left, 1156,
                   684 - reading_footer::device_left, 24, 3);
        } else {
            outline(556, 1165, 32, 17, 2);
            canvas.fill_rect(588, 1170, 3, 7, 1);
            if (battery_percent) canvas.fill_rect(560, 1169, *battery_percent * 24 / 100, 9, 3);
            text(battery_percent ? std::to_string(*battery_percent) + "%" : "—", 600, 1156, 24, 80, 3);
        }
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
        invalidate_counting();
        if (!keep_prepared_frames) { clear_body_cache(); clear_prefetch(); }
        const auto &spine = document.spine();
        chapter_pages.assign(spine.size(), {});
        counted.assign(spine.size(), false);
        counted_live.assign(spine.size(), false);
        for (std::size_t i = 0; i < spine.size(); ++i) counted[i] = !spine[i].linear;
        counting_index = counting_blocked = not_counting;
        release_counting();
        pages_failed = pages_dirty = pages_cache_pending = false;
        pages_cache_disabled = false;
        font_scan_turn = true;
        pages_complete = std::find(counted.begin(), counted.end(), false) == counted.end();
        pages_name.clear();
        if (builtin_body) font_identity = graphics::Font::fallback_identity();
        if (!async_content && font_identity.empty()) font_identity = sha256_file(body_font_path());
        if (!font_identity.empty()) load_pages_cache();
    }
    void load_pages_cache() {
        const auto& spine = document.spine();
        struct stat book {}, font {};
        const std::string font_file = builtin_body ? "" : font_fallback ? "Fonts/LXGWWenKai-Regular.ttf" : effective_font;
        if (stat((root + "/" + book_path).c_str(), &book) != 0 ||
            (!font_file.empty() && stat((root + "/" + font_file).c_str(), &font) != 0)) {
            pages_cache_disabled = true;
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
        pages_header = std::string("PPPAGES3\n") + revision_for(book_path, document.metadata()) + ":pagination-v3\n" + book_path + "\n" + font_file + "\n" + geometry +
            document.metadata().book_id + "\n" + font_identity + "\n";
        if (async_content) { pages_cache_pending = true; return; }
        if (auto cached = read_page_cache(store, pages_name, pages_header, spine)) adopt_pages(std::move(*cached));
    }
    void adopt_pages(PageCache cached) {
        std::size_t combined = page_index_bytes() + cached.pages.capacity() * sizeof(PageIndices::value_type);
        for (const auto& parts : cached.pages) {
            combined += parts.capacity() * sizeof(std::vector<std::uint32_t>);
            for (const auto& starts : parts) combined += starts.capacity() * sizeof(std::uint32_t);
        }
        if (combined > max_pages_bytes) return;
        // A late identity check must not replace chapters already counted in this session.
        for (std::size_t i = 0; i < counted.size(); ++i) {
            if (counted[i] || (i == counting_index && !cached.counted[i])) {
                cached.pages[i] = std::move(chapter_pages[i]);
                cached.counted[i] = counted[i];
            }
        }
        chapter_pages = std::move(cached.pages); counted = std::move(cached.counted);
        counted_live = counted;
        if (counting_index != not_counting && counted[counting_index]) {
            release_counting(); counting_index = not_counting;
        }
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
        if (async_content || !pages_dirty || pages_name.empty() || pages_cache_disabled) return;
        if (!write_page_cache(store, pages_name, pages_header, chapter_pages, counted)) pages_cache_disabled = true;
        pages_dirty = false;
    }
    static constexpr std::size_t cache_target = 4U * 1024 * 1024;
    std::uint64_t cache_evictions = 0;
    CacheUsage cache_usage() const {
        CacheUsage usage;
        usage.body_bitmap = body_canvas ? body_canvas->pixels().capacity() : 0;
        if (next_page) usage.next_layout = next_page->glyphs.capacity() * sizeof(graphics::PositionedGlyph) +
            next_page->lines.capacity() * sizeof(graphics::TextLine);
        usage.next_bitmap = next_bitmap ? next_bitmap->pixels().capacity() : 0;
        usage.bitmap_hits = bitmap_hits;
        usage.page_index = page_index_bytes();
        for (const auto& cover : covers) usage.cover_pixels += cover.pixels.capacity();
        if (detail_cover) usage.cover_pixels += detail_cover->pixels().capacity();
        if (custom_sleep_image) usage.cover_pixels += custom_sleep_image->pixels().capacity();
        usage.ui_glyph_bytes = ui.cached_glyph_bytes(); usage.body_glyph_bytes = body.cached_glyph_bytes();
        usage.ui_glyphs = ui.cached_glyphs(); usage.body_glyphs = body.cached_glyphs();
        usage.ui_advances = ui.cached_advances(); usage.body_advances = body.cached_advances();
        usage.evictions = cache_evictions; usage.target = cache_target;
        usage.retained = usage.body_bitmap + usage.next_layout + usage.next_bitmap +
            usage.page_index + usage.cover_pixels + usage.ui_glyph_bytes + usage.body_glyph_bytes +
            (usage.ui_advances + usage.body_advances) * (sizeof(std::uint64_t) + sizeof(int));
        return usage;
    }
    void reclaim_caches() {
        if (view != View::Reading || display.busy() || content_loading ||
            (counting_job && (counting_job->request.kind == ContentJob::Kind::SleepCover || counting_job->request.kind == ContentJob::Kind::LockScreen))) return;
        const auto pressure = [&] {
            if (cache_usage().retained > cache_target) return true;
#ifdef ESP_PLATFORM
            return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 1536U * 1024 ||
                   heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 512U * 1024;
#else
            return false;
#endif
        };
        if (!pressure()) return;
        // The counting session is rebuilt on demand, so it goes before any counted pages.
        if (yield_counting_session()) { defer_counting(); return; }
        const StageTimer timer{"cache_reclaim"};
        if (!covers.empty()) { covers.clear(); ++cache_evictions; }
        bool dropped = false;
        for (std::size_t i = chapter_pages.size(); i-- > 0 && pressure();) {
            if (i == chapter_index || chapter_pages[i].empty()) continue;
            decltype(chapter_pages)::value_type().swap(chapter_pages[i]);
            counted[i] = !document.spine()[i].linear; counted_live[i] = false;
            dropped = true; ++cache_evictions;
        }
        if (dropped) {
            invalidate_counting(); release_counting(); counting_index = not_counting;
            defer_counting(); pages_cache_pending = pages_dirty = false;
            pages_complete = std::find(counted.begin(), counted.end(), false) == counted.end();
        }
        if (pressure() && (next_page || next_bitmap)) {
            clear_prefetch(); prefetch_done = true; ++cache_evictions;
        }
        if (pressure()) {
            if (counting_job && counting_job->request.generation == counting_generation) {
                invalidate_counting(); release_counting(); counting_index = not_counting;
            }
            const auto before = ui.cached_glyph_bytes() + body.cached_glyph_bytes();
            ui.trim_glyph_cache(128U * 1024); body.trim_glyph_cache(256U * 1024);
            if (ui.cached_glyph_bytes() + body.cached_glyph_bytes() < before) ++cache_evictions;
            defer_counting();
        }
    }
    bool background_content_ready() const {
        return !sleep_cover_deadline && !power_menu_wait && !session.needs_save() && view == View::Reading && !image_viewing && has_book() && card_available && !usb_prepared &&
               !display.busy() && !deferred && !save_job && !bookmark_job && !foreground_job && !pending_content && !body_images_pending &&
               !cover_job && !pending_covers;
    }
    bool counting_due() const {
        return background_content_ready() &&
               (!pages_complete || (!pages_cache_disabled && (pages_cache_pending || pages_dirty || (async_content && font_identity.empty())))) && !pages_failed &&
               (counting_blocked != chapter_index || std::chrono::steady_clock::now() >= counting_retry);
    }
    /** Frees the chapter loaded for counting; its partial count resumes after a reload. */
    void release_counting() {
        if (counting_job) counting_job->cancel();
        else if (counting_session) { release(counting_session->chapter); counting_session->loaded = false; }
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
        counting_blocked = not_counting;
        if (async_content && font_identity.empty() && !pages_cache_disabled && (font_scan_turn || pages_complete)) {
            if (!counting_job) {
                if (!font_scan) font_scan = std::make_shared<FileDigest>(body_font_path());
                ContentJob::Request request;
                request.font = body_font_path(); request.kind = ContentJob::Kind::FontIdentity;
                request.font_scan = font_scan;
                request.generation = counting_generation;
                counting_job = std::make_unique<ContentJob>(std::move(request));
                if (!counting_job->start()) { counting_job.reset(); defer_counting(); }
                else font_scan_turn = false;
            }
            return;
        }
        if (async_content && !font_identity.empty() && !pages_cache_disabled && (pages_cache_pending || pages_dirty)) {
            if (counting_job) return;
#ifdef ESP_PLATFORM
            // The encoded payload and expanded index are each bounded to kPageIndexBytes.
            if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 2 * kPageIndexBytes + 256U * 1024 ||
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < kPageIndexBytes + 64U * 1024) {
                if (yield_counting_session()) return;
                pages_cache_disabled = true; pages_cache_pending = pages_dirty = false;
                return;
            }
#endif
            ContentJob::Request request;
            request.kind = pages_cache_pending ? ContentJob::Kind::PageCacheRead : ContentJob::Kind::PageCacheWrite;
            request.root = root; request.cache_name = pages_name; request.cache_header = pages_header;
            request.generation = counting_generation;
            if (pages_cache_pending) request.spine = spine;
            else request.page_cache = PageCache{chapter_pages, counted};
            counting_job = std::make_unique<ContentJob>(std::move(request));
            if (!counting_job->start()) {
                counting_job.reset(); pages_cache_disabled = true; pages_cache_pending = pages_dirty = false;
            }
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
            font_scan_turn = true;
            // No job runs here, so the session members may be read. The memory check covers the
            // load of a new part and assumes that the previous part is no longer held.
            const bool part_loaded = counting_session && counting_session->loaded &&
                counting_session->chapter_index == counting_index && counting_session->part == counting_part;
            if (!part_loaded) {
                release_counting();
                if (!counting_memory(spine[counting_index].bytes)) { counting_session.reset(); defer_counting(); return; }
            }
            ContentJob::Request request;
            request.root = root; request.path = root + "/" + book_path;
            request.font = body_font_path();
            request.identity = document.metadata(); request.layout = layout(settings);
            request.generation = counting_generation; request.chapter = counting_index;
            request.part = counting_part; request.offset = counting_offset;
            if (!counting_session) counting_session = std::make_shared<PaginationSession>();
            request.session = counting_session;
            counting_job = std::make_unique<ContentJob>(std::move(request));
            if (!counting_job->start()) { counting_job.reset(); defer_counting(); }
            return;
        }
        const bool in_place = counting_index == chapter_index && counting_part == chapter.part &&
                              chapter.resource == spine[counting_index].resource;
        if (in_place) release_counting();
        else if (!counting_loaded || counting_chapter.part != counting_part ||
                 counting_chapter.resource != spine[counting_index].resource) {
            release_counting();
            if (!counting_memory(spine[counting_index].bytes)) {
                defer_counting();
                return;
            }
            std::string detail;
            const StageTimer load_timer{"chapter_counting"};
            if (load_content(store, document, counting_index, counting_chapter, detail, ChapterLoad::Layout, counting_part, stop))
                counting_loaded = true;
            else if (detail == kChapterCancelled)
                return;
            else if (memory_error(detail))
                defer_counting();
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
    void draw_image_error(const graphics::ImageBox& box) {
        outline(box.x, box.y + 12, box.width, box.height - 24, 2);
        center("图片无法显示", box.x + 8, box.y + box.height / 2 - 36, box.width - 16, 28);
        center("图片缺失、损坏或内存不足", box.x + 8, box.y + box.height / 2 + 16, box.width - 16, 22, 3);
    }
    bool draw_page_content(const graphics::Page& planned, const graphics::LayoutSpec& spec, bool progressive = false) {
        const StageTimer timer{"page_draw"};
        if (!planned.images.empty()) {
            if (image_prepared) {
                if (prepared_image && prepared_image_error.empty()) canvas = *prepared_image;
                for (const auto& box : planned.images)
                    if (!prepared_image || !prepared_image_error.empty() ||
                        std::find(prepared_image_failures.begin(), prepared_image_failures.end(), box.begin) != prepared_image_failures.end())
                        draw_image_error(box);
                return graphics::render_page(canvas, body, planned);
            }
            if (async_content && progressive) {
                body_images_pending = true;
                for (const auto& box : planned.images) {
                    outline(box.x, box.y + 12, box.width, box.height - 24, 2);
                    center("正在准备图片…", box.x + 8, box.y + box.height / 2 - 14, box.width - 16, 24, 3);
                }
                return graphics::render_page(canvas, body, planned);
            }
            const bool cacheable = planned.images.size() == 1 && planned.glyphs.empty() &&
                                   planned.images.front().height == spec.height;
            const auto cache_name = cacheable ? image_cache_key(document.metadata().book_id, planned.images.front().resource, spec) : "";
            if (cacheable && body_cache_room()) {
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
            bool all_drawn = true;
            for (const auto& box : planned.images) {
                // Foreground images are prepared by a worker; overlays do not start a decoder.
                if (async_content) { draw_image_error(box); all_drawn = false; continue; }
                std::vector<std::uint8_t> encoded;
                std::string detail;
                if (box.resource.empty() || !document.read_image(box.resource, encoded, detail) ||
                    !graphics::render_image(canvas, encoded, box.x, box.y, box.width, box.height, detail)) {
                    draw_image_error(box);
                    all_drawn = false;
                }
            }
            if (cacheable && all_drawn && body_cache_room()) {
                    std::string pixels(reinterpret_cast<const char*>(canvas.data()), canvas.pixels().size());
                    pixels += md5_hex(pixels);
                    store.save_cache(cache_name, pixels);
            }
        }
        return graphics::render_page(canvas, body, planned);
    }
    bool body_reusable(std::size_t start, const graphics::LayoutSpec& spec) const {
        return body_canvas && start == page.start && same_layout(body_spec, spec);
    }
    bool render_body(std::size_t start) {
        const StageTimer timer{"body_draw"};
        const auto spec = layout(settings, link_return ? 144 : 64);
        if (body_reusable(start, spec)) {
            canvas = *body_canvas;
            const bool marked = bookmarked(page.start, page.end);
            if (body_bookmarked != marked) {
                // The ribbon sits in the blank top margin, above text and image layout.
                canvas.fill_rect(bookmark_cell_left + 38, 0, 24, 44, 15);
                if (marked) bookmark_ribbon(bookmark_cell_left + 50, 0, 44, true);
                *body_canvas = canvas;
                body_bookmarked = marked;
            }
            links = body_links;
            offset = page.start;
            footer();
            return true;
        }
        const bool same_spec = same_layout(next_spec, spec);
        const bool prefetched = next_page && same_spec && next_page->start == readable_offset(start);
        auto candidate = prefetched ? std::move(*next_page) : plan_page(start, spec);
        auto bitmap = prefetched ? std::move(next_bitmap) : nullptr;
        clear_prefetch();
        if (!candidate.error.empty() || candidate.end <= candidate.start) {
            error("本页无法排版。" + reason(candidate.error) + "。");
            return false;
        }
        // The built-in subset covers common characters only; the page stays readable and the
        // missing ones draw as the font's .notdef box. The notice applies to this page alone.
        const bool missing = std::any_of(candidate.glyphs.begin(), candidate.glyphs.end(),
            [this](const auto &glyph) { return !body.contains(glyph.scalar); });
        if (missing) message = kMissingNotice;
        else if (message == kMissingNotice) message.clear();
        canvas.clear();
        body_images_pending = false;
        if (bitmap) {
            canvas = std::move(*bitmap);
            ++bitmap_hits;
        }
        else if (!draw_page_content(candidate, spec, true)) {
            error("正文绘制失败。" + reason(body.error()) + "。");
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
                        body.size(glyph.font_px);
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
        if (bookmarked(page.start, page.end)) bookmark_ribbon(bookmark_cell_left + 50, 0, 44, true);
        if (body_cache_room()) {
            body_canvas = canvas; body_spec = spec; body_links = links;
            body_bookmarked = bookmarked(page.start, page.end);
        }
        else clear_body_cache();
        return true;
    }
    bool show_page(std::size_t start) {
        const auto spec = layout(settings, link_return ? 144 : 64);
        const bool reused = body_reusable(start, spec);
        const bool hit = next_page && next_bitmap && next_page->start == readable_offset(start) && same_layout(next_spec, spec);
        const StageTimer timer{hit ? "body_prepare_hit" : reused ? "body_prepare_reuse" : "body_prepare_miss", 0};
        if (display.busy())
            return false;
        load_fonts();
        if (inline_image_pending) {
            const auto& request = pending_content ? *pending_content : foreground_job->request;
            if (request.offset != readable_offset(start) || request.chapter != chapter_index ||
                request.part != chapter.part || !same_layout(request.layout, spec)) abandon_content();
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
        inline_image_after = std::chrono::steady_clock::time_point::max();
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
        // The font notice is shown on the page where the book opened; later pages show the position.
        if (message == kBuiltinFontNotice || message == kFallbackFontNotice || message == kWeightUnavailableNotice) message.clear();
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
        const char *labels[] = {"目录", "排版", "同步", "刷新模式", "全刷"};
        const int ids[] = {Contents, Typography, Sync, Refresh, FullRefresh};
        for (int i = 0; i < 5; ++i) {
            // Four cells of 137 pixels leave 136 for the last one.
            const int x = i * 137, width = std::min(137, 684 - x);
            if (i) canvas.line(x, toolbar_top, x, 1215, 1, 2);
            tool_icon(ids[i], x + 68, 1126);
            const int size = ids[i] == Refresh ? 26 : 28;
            center(labels[i], x, 1160 + (28 - size) / 2, width, size);
            hits.push_back({x, toolbar_top, width, 136, ids[i]});
        }
        if (!notice().empty()) {
            canvas.fill_rect(24, 1012, 636, 56, 15);
            note(message, 1024);
        }
        hits.push_back({0, 104, 684, toolbar_top - 104, Dismiss});
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
    /**
     * Whole-book percentage at which a contents entry starts, from counted pages or, until then,
     * from chapter sizes. -1 for an entry inside a chapter, whose offset is unknown until that
     * chapter is loaded.
     */
    int toc_percent(std::size_t index) const {
        const auto &toc = document.toc();
        const auto &spine = document.spine();
        std::size_t target;
        if (!document.chapter_index_for(toc[index].resource, target) || target >= spine.size()) return -1;
        if (!toc[index].fragment.empty())
            for (std::size_t i = 0; i < index; ++i)
                if (toc[i].resource == toc[index].resource) return -1;
        std::uint64_t before = 0, total = 0;
        for (std::size_t i = 0; i < spine.size(); ++i) {
            if (!spine[i].linear) continue;
            std::uint64_t weight = spine[i].bytes;
            if (pages_complete) {
                weight = 0;
                for (const auto &starts : chapter_pages[i]) weight += starts.size();
            }
            total += weight;
            if (i < target) before += weight;
        }
        return total ? static_cast<int>(before * 100 / total) : -1;
    }
    static constexpr int contents_top = 350, contents_switch_height = 72, bookmark_row = 112;
    static constexpr std::size_t bookmarks_per_screen =
        (footer_zone - contents_top - 100 - contents_switch_height) / bookmark_row;
    /** Selects the table of contents or the bookmark list below the panel tabs. */
    void contents_switch(int top) {
        canvas.fill_rect(0, top, 684, contents_switch_height, 15);
        const auto count = bookmarks_usable() && !bookmarks.bookmarks.empty()
                               ? " " + std::to_string(bookmarks.bookmarks.size()) : std::string();
        const std::string names[] = {"目录", "书签" + count};
        const int ids[] = {ContentsToc, ContentsBookmarks};
        for (int i = 0; i < 2; ++i) {
            const bool active = (i == 1) == contents_bookmarks;
            const int x = i * 342;
            center(names[i], x, top + 20, 342, 24, active ? 1 : 3, active);
            if (active) canvas.fill_rect(x + 131, top + contents_switch_height - 8, 80, 4, 1);
            hits.push_back({x, top, 342, contents_switch_height, ids[i]});
        }
        canvas.line(342, top + 16, 342, top + contents_switch_height - 17, 12, 2);
        canvas.line(0, top + contents_switch_height - 1, 683, top + contents_switch_height - 1, 1, 2);
    }
    void draw_bookmarks(int list_top) {
        constexpr int row = bookmark_row;
        constexpr auto per_page = bookmarks_per_screen;
        const auto &items = bookmarks.bookmarks;
        const auto problem = bookmark_problem();
        if (!problem.empty() || items.empty()) {
            wrapped(problem.empty() ? "还没有书签。在正文工具页点右上角的“加书签”，可标记当前页。" : problem,
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
            title_lines(item.chapter.empty() ? "书签 " + std::to_string(item.sequence) : item.chapter,
                        36, y + 18, 600 - width - 24, 26, selected, 1, ink);
            text(position, 648 - width, y + 20, 22, width, selected ? 15 : 3);
            title_lines(item.excerpt.empty() ? "（本页没有文字）" : item.excerpt, 36, y + 64, 612, 22, false, 1, selected ? 15 : 3);
            if (!selected) canvas.line(0, y + row - 1, 683, y + row - 1, 1, 2);
            hits.push_back({0, y, 684, row, BookmarkBase + static_cast<int>(i)});
            y += row;
        }
        footer_bar("", bookmark_page, pages);
    }
    void draw_contents() {
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
        auto row_height = [&](std::size_t index) {
            return std::max(92, std::min(wrapped(toc[index].label, 0, 0, title_width(index), 26, false), 80) + 24);
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
            title_lines(item.label, 36 + indent, y + (h > 92 ? 14 : 28), title_width(i), item.depth ? 24 : 28, selected, 2, ink);
            const int percent = toc_percent(i);
            const auto position = selected ? "当前 · " + book_percent() : percent < 0 ? std::string() : std::to_string(percent) + "%";
            const int right = parent ? 560 : 648, width = text_width(position, 22);
            text(position, right - width, y + (h - 22) / 2 - 4, 22, width, selected ? 15 : 3);
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
    void draw_typography() {
        view = View::Typography;
        hits.clear();
        canvas.clear();
        load_fonts(&draft);
        const auto spec = layout(draft, 64, preview_bottom);
        const auto preview = plan_page(offset, spec);
        if (async_content && !image_prepared && preview.error.empty() && !preview.images.empty()) {
            // The worker prepares the image; the panel stays visible and only cancelling is accepted.
            begin_image(offset, preview, true);
            text("正在准备图片…", 36, 300, 26, 400, 3);
            button(Dismiss, "取消", 262, 368, 160, 64, false, 24);
            loading_cancel = hits.back();
        } else {
            if (!preview.error.empty() || !draw_page_content(preview, spec))
                text("预览暂不可用", 36, 96, 28);
        }
        const bool changed = typography_changed();
        tabs(656, Typography);
        canvas.fill_rect(24, 605, 636, 44, 15);
        text(changed ? "预览 · 尚未应用" : "当前排版 · 此书", 36, 614, 22, 432, 3);
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
                outline(x, y, 150, 80, parameter == ids[i] ? 3 : 1, parameter == ids[i] ? 1 : 9);
                center(name(ids[i]), x, y + 5, 150, 22);
                center(value(ids[i]), x + 6, y + 37, 138, 24, 1, true);
            }
            hits.push_back({x, y, 150, 80, ids[i]});
        }
        auto can_step = [&](int direction) {
            switch (parameter) {
            case FontChoice: {
                const auto at = std::find(fonts.begin(), fonts.end(), draft.font);
                return direction < 0 ? at != fonts.begin() : at != fonts.end() && at + 1 != fonts.end();
            }
            case FontWeight: return direction < 0 ? draft.font_weight != (light_available(draft) ? "light" : "regular") : draft.font_weight != "bold";
            case FontSize: return direction < 0 ? draft.font_size_px > 28 : draft.font_size_px < 56;
            case LineHeight: return direction < 0 ? draft.line_height > 1.35001 : draft.line_height < 1.64999;
            case Margin: return direction < 0 ? draft.margin_x_px > 24 : draft.margin_x_px < 52;
            case Gap: return direction < 0 ? draft.paragraph_gap_lines > 0.00001 : draft.paragraph_gap_lines < 0.99999;
            default: return direction < 0 ? draft.indent_chars > 0 : draft.indent_chars < 2;
            }
        };
        if (can_step(-1)) button(Minus, "−", 24, 976, 150, 88, false, 40);
        else disabled_button("−", 24, 976, 150, 88);
        center(value(parameter), 186, 1000, 312, 30, 1, true);
        if (can_step(1)) button(Plus, "+", 510, 976, 150, 88, false, 40);
        else disabled_button("+", 510, 976, 150, 88);
        if (!font_fallback && !weight_unavailable) button(Scope, "设为阅读默认", 24, 1088, 312, 96, false, 26);
        else disabled_button("设为阅读默认", 24, 1088, 312, 96);
        if (!font_fallback && !weight_unavailable && changed) button(Apply, "应用到此书", 348, 1088, 312, 96, true, 26);
        else disabled_button(font_fallback || weight_unavailable ? "无法应用" : "已应用", 348, 1088, 312, 96);
        if (font_fallback || weight_unavailable) {
            canvas.fill_rect(24, preview_bottom - 70, 636, 58, 15);
            note(weight_unavailable && !font_fallback ? kWeightUnavailableNotice : "所选字体不可用，预览使用替代字体。", preview_bottom - 60);
        }
    }
    static std::string weight_name(const std::string &weight) {
        return weight == "light" ? "细" : weight == "medium" ? "中粗" : weight == "bold" ? "粗" : "常规";
    }
    /** Only explicit sibling weight files are selected; no directory scan occurs during preview. */
    std::string weight_font(const ReaderSettings &values) const {
        if (values.font_weight == "regular") return values.font;
        const auto dot = values.font.find_last_of('.');
        const auto dash = values.font.find_last_of('-');
        if (dot == std::string::npos || dash == std::string::npos || dash > dot) return values.font;
        const auto suffix = values.font.substr(dash + 1, dot - dash - 1);
        if (suffix != "Regular" && suffix != "Light" && suffix != "Medium" && suffix != "Bold") return values.font;
        const auto weight = values.font_weight == "light" && values.font_size_px >= 34 ? "Light" : values.font_weight == "medium" ? "Medium" :
                            values.font_weight == "bold" ? "Bold" : "Regular";
        const auto path = values.font.substr(0, dash + 1) + weight + values.font.substr(dot);
        if (std::find(fonts.begin(), fonts.end(), path) != fonts.end()) return path;
        if (values.font_weight == "medium" || values.font_weight == "bold") {
            const auto regular = values.font.substr(0, dash + 1) + "Regular" + values.font.substr(dot);
            if (std::find(fonts.begin(), fonts.end(), regular) != fonts.end()) return regular;
        }
        return values.font;
    }
    bool light_available(const ReaderSettings &values) const {
        auto light = values; light.font_weight = "light";
        return values.font_size_px >= 34 && (weight_font(light) != values.font || values.font.find("-Light.") != std::string::npos);
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
        return values.sleep_screen == "custom" ? "自定义图片" : values.sleep_screen == "reading" ? "保留正文" : values.sleep_screen == "simple" ? "简洁屏保" : "书籍封面";
    }
    static int refresh_interval(const ReaderSettings &values) {
        return values.refresh_preset == "fast" ? values.fast_full_refresh_pages : values.balanced_full_refresh_pages;
    }
    /** One radio row. framed adds the outline and the state tag of a choice that needs saving. */
    void choice(int id, const std::string &title, const std::string &description, int y, bool selected,
                bool pending_choice = false, bool framed = true) {
        if (!framed) {}
        else if (selected && pending_choice) outline(24, y, 636, 116, 2);
        else if (selected) outline(24, y, 636, 116, 3);
        for (int row = -18; row <= 18; ++row)
            for (int col = -18; col <= 18; ++col) {
                const int radius = row * row + col * col;
                if ((radius <= 324 && radius >= 225) || (selected && radius <= 100))
                    canvas.pixel(62 + col, y + 48 + row, 1);
            }
        text(title, 102, y + 16, 28, 430, 1, true);
        text(description, 102, y + 65, 22, 540, 3);
        if (selected && framed) {
            canvas.fill_rect(540, y + 20, 102, 36, 1);
            center(pending_choice ? "待保存" : "当前", 540, y + 22, 102, 22, 15);
        }
        hits.push_back({24, y, 636, 116, id});
    }
    /**
     * Draws the reading page behind a panel that starts at top and clears the panel area. With
     * live false the header only names the book, for a page that accepts panel controls alone.
     */
    bool panel_background(int top, bool live = true) {
        hits.clear();
        if (has_book()) {
            load_fonts();
            if (!render_body(offset)) return false;
            for (const auto &line : page.lines)
                if ((line.top < 104 && line.bottom > 104) || (line.top < top && line.bottom > top))
                    canvas.fill_rect(0, line.top, 684, line.bottom - line.top + 1, 15);
            if (live) reading_header(true);
            else header(document.metadata().title, 0);
        } else {
            canvas.clear();
            header("显示与刷新", Back, "设置");
        }
        canvas.fill_rect(0, top, 684, 1216 - top, 15);
        return true;
    }
    void draw_refresh() {
        view = View::Refresh;
        if (!panel_background(refresh_from_settings ? 502 : 502 - panel_strip)) return;
        if (refresh_from_settings) {
            hits.clear();
            header("显示与刷新", Back, "设置");
            text("阅读默认 · 本书覆盖项保持不变", 36, 536, 24, 612, 3);
            canvas.line(0, 502, 683, 502, 1, 3);
        } else tabs(502, Refresh);
        const auto &values = edit_snapshot.config.settings;
        choice(RefreshClear, "清晰", "每页执行全刷，减少残影", 616, values.refresh_preset == "clear");
        choice(RefreshBalanced, "均衡", "灰阶更新，定期全刷", 752, values.refresh_preset == "balanced");
        choice(RefreshFast, "快速", "优先响应速度，可能更易留痕", 884, values.refresh_preset == "fast");
        button(FullRefresh, "立即全刷", 36, 1020, 294, 96);
        button(PageEffects, "翻页效果", 354, 1020, 294, 96);
        if (values.refresh_preset == "clear") note("自动全刷间隔：每页", 1150);
        else {
            text("自动全刷间隔", 36, 1150, 24, 360);
            text("每 " + std::to_string(refresh_interval(values)) + " 页 ›", 490, 1150, 24, 170);
            hits.push_back({24, 1128, 636, 88, RefreshInterval});
        }
    }
    void draw_page_effects() {
        view = View::PageEffects;
        hits.clear(); canvas.clear(); header("翻页效果", Back, "刷新");
        text("实验功能 · 本次启动有效", 36, 144, 26, 612, 3);
        choice(EffectOff, "关闭", "沿用当前刷新模式", 220, page_effect == PageEffect::Off);
        choice(EffectA, "文字转页 A", "仅左对齐，保留全部驱动动作", 356, page_effect == PageEffect::TextTurnA);
        choice(EffectB, "文字转页 B", "A + 未变化像素不驱动", 492, page_effect == PageEffect::TextTurnB);
        choice(EffectC, "文字转页 C", "B + 去掉深色目标的前导擦白", 628, page_effect == PageEffect::TextTurnC);
        choice(EffectRipple, "水波纹", "条带依次展开，翻页可能更慢", 764, page_effect == PageEffect::Ripple);
        wrapped("仅用于正文非全刷更新。清晰模式每页全刷，不播放效果。", 36, 936, 612, 25, 3);
        wrapped("效果、残影和灰阶均待真机验证。按全刷键可清理当前画面。", 36, 1040, 612, 25, 3);
    }
    void draw_refresh_confirm() {
        draw_refresh();
        if (view == View::Error) return;
        view = View::RefreshConfirm;
        hits.clear();
        if (has_book() && !refresh_from_settings) header(document.metadata().title, 0);
        else header("显示与刷新", 0);
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
        hits.clear(); canvas.clear(); header("自动全刷间隔", Back, "刷新");
        text(refresh_name(draft) + "模式", 36, 170, 32, 612, 1, true);
        wrapped("间隔越短，清理残影越频繁。此设置不影响 K2 手动全刷。", 36, 246, 612, 26);
        button(IntervalLess, "−", 36, 440, 120, 96);
        center("每 " + std::to_string(refresh_interval(draft)) + " 页", 168, 460, 348, 34);
        button(IntervalMore, "+", 528, 440, 120, 96);
        note("可设为 1 至 30 页。", 582);
        button(Back, "取消", 24, 1080, 306, 96);
        button(IntervalSave, "保存间隔", 354, 1080, 306, 96, true);
    }
    static constexpr int settings_pitch = 116;
    /**
     * A row with a title and a summary, 132 pixels high unless height says otherwise. A zero id
     * draws a description without a hit.
     */
    void settings_row(int id, const std::string &title, const std::string &value, int y, std::uint8_t rule = 12,
                      int height = 132) {
        const int lift = (132 - height) / 2;
        text(title, 36, y + 20 - lift, 28, 554, 1, true);
        text(value, 36, y + 70 - lift, 22, 554, 3);
        canvas.line(24, y + height - 1, 660, y + height - 1, rule, 2);
        if (!id) return;
        text("›", 620, y + 42 - lift, 28, 40);
        hits.push_back({24, y, 636, height, id});
    }
    bool wifi_enabled() const {
        // Preserve the admitted permission during a request; an upload session also owns card writes.
        if (portal_live || network_worker_id || update_worker) return true;
        if (bookmark_job) return false;
        const auto cfg = store.load_settings();
        return cfg.diagnostics.empty() && cfg.settings.wifi_enabled;
    }
    static std::string quoted_name(const std::string& name) { return "“" + name + "”"; }
    bool wifi_online() const {
        return wifi_status.phase == WifiPhase::Connected || wifi_status.phase == WifiPhase::Clock ||
               wifi_status.phase == WifiPhase::Request;
    }
    /** Connection state for the Wi-Fi page title and the settings row. */
    std::string wifi_summary() const {
        if (!wifi_enabled()) return "Wi-Fi 已关闭";
        if (portal_live && !portal_station) return portal_closing ? "正在结束…" : "设备热点已开启";
        switch (wifi_status.phase) {
        case WifiPhase::Connecting:
            return wifi_status.ssid.empty() ? "正在连接…" : "正在连接" + quoted_name(wifi_status.ssid);
        case WifiPhase::Clock:
        case WifiPhase::Request:
        case WifiPhase::Connected: return "已连接" + quoted_name(wifi_status.ssid);
        case WifiPhase::Hotspot: return "设备热点已开启";
        case WifiPhase::Closing: return "正在结束…";
        default: return "未连接";
        }
    }
    /** The current link, or the saved connection mode, for the sync panel. */
    std::string network_label() const {
        if (wifi_online()) return "已连接" + quoted_name(wifi_status.ssid);
        std::optional<provision::SavedWifi> pinned;
        std::string detail;
        if (!provision::load_selected_network(root, pinned, detail)) return "配置无效";
        return pinned ? "指定 " + pinned->ssid : "自动";
    }
    /**
     * Networks for one connection under the saved mode: the pinned network alone, or every saved
     * network for automatic selection. automatic overrides a pinned mode for this call only.
     * networks is empty, with problem set, when nothing is saved or network.toml is unusable.
     */
    WifiTargets wifi_targets(std::string& problem, bool automatic = false) const {
        WifiTargets targets;
        std::vector<provision::SavedWifi> saved;
        std::optional<provision::SavedWifi> pinned;
        std::string detail;
        if (!provision::load_networks(root, saved, detail) || !provision::load_selected_network(root, pinned, detail))
            problem = "网络配置无效，请检查 PicoPage/network.toml。";
        else if (saved.empty()) problem = "还没有保存 Wi-Fi 网络，请先在 Wi-Fi 设置中添加。";
        else if (pinned && !automatic) targets.networks.push_back(*pinned);
        else {
            targets.automatic = true;
            targets.networks.assign(saved.begin(), saved.end());
        }
        return targets;
    }
    void wifi_link(int y) {
        text("Wi-Fi 状态 ›", 432, y, 24, 216);
        hits.push_back({420, y - 12, 240, 52, WifiSettings});
    }
    void draw_wifi() {
        if (!portal_live && !bookmark_io_ready()) return;
        view = View::WifiSettings;
        hits.clear(); canvas.clear();
        // A running upload session owns card writes. This page uses only admitted state and snapshots.
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
        wifi_target = pinned ? pinned->ssid : "";
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
                 : !wifi_target.empty() ? "指定" + quoted_name(wifi_target) + "，不改用其他网络"
                 : online ? "自动 · 信号最强" : "自动连接信号最强的已保存网络", 36, 186, 22, 612, 3);
            value_row(WifiToggle, "Wi-Fi", "", enabled ? "开" : "关", 236, 104, true, false);
            if (enabled) {
                value_row(WifiModeOpen, "连接方式", "", wifi_target.empty() ? "自动" : "指定", 340);
                value_row(WifiManage, "已保存网络", "", saved_count + " 个", 444);
                value_row(WifiAdd, "添加网络", "", "", 548);
                // One action for the current state: a link or an attempt can be ended. A kept
                // connection that an access point session suspended is offline and can be resumed.
                if (online || wifi_status.phase == WifiPhase::Connecting ||
                    (wifi_worker && wifi_kind == WifiOperation::Kind::Connect))
                    button(WifiDisconnect, "断开", 24, 700, 636, 96);
                else if (wifi_saved.empty()) disabled_button("立即连接", 24, 700, 636, 96);
                else button(WifiConnect, "立即连接", 24, 700, 636, 96);
                if (!online)
                    wrapped("同步和传书会自动联网，用完断开。立即连接后保持在线，直到断开或休眠。", 36, 824, 612, 24, true, 3);
            }
            wrapped(notice_text, 36, 1060, 612, 24);
            break;
        }
        case WifiPage::Mode:
            header("连接方式", Back, wifi_root == WifiPage::Mode ? "同步" : "Wi-Fi");
            // A tap applies the mode at once, so the radio mark alone shows the current one.
            choice(WifiModeAuto, "自动", "信号最强的已保存网络，连不上时试下一个", 130, wifi_target.empty(), false, false);
            text("指定", 36, 272, 22, 612, 3);
            if (wifi_saved.empty()) note("还没有已保存的网络。", 316);
            for (std::size_t i = 0; i < wifi_saved.size(); ++i)
                choice(WifiModeBase + static_cast<int>(i), wifi_saved[i].ssid, "只连这个网络",
                       312 + static_cast<int>(i) * 124, wifi_saved[i].ssid == wifi_target, false, false);
            wrapped("指定后只连这个网络。它被忘记后回到自动。", 36,
                    336 + static_cast<int>(std::max<std::size_t>(wifi_saved.size(), 1)) * 124, 612, 24, true, 3);
            wrapped(notice_text, 36, 1060, 612, 24);
            break;
        case WifiPage::Saved:
            header("已保存网络", Back, "Wi-Fi");
            note("已保存 " + saved_count + " 个，最多 " + std::to_string(provision::saved_limit) + " 个", 130);
            for (std::size_t i = 0; i < wifi_saved.size(); ++i)
                entry_row(WifiSavedBase + static_cast<int>(i), wifi_saved[i].ssid, "",
                          wifi_saved[i].ssid == wifi_target ? "指定" : "", 180 + static_cast<int>(i) * 112, 112);
            if (!full) button(WifiAdd, "添加网络", 24, 212 + static_cast<int>(wifi_saved.size()) * 112, 636, 96);
            wrapped(notice_text, 36, 1060, 612, 24);
            break;
        case WifiPage::Detail:
            header("网络", Back, "已保存网络");
            title_lines(wifi_choice, 36, 132, 612, 34, true, 1);
            text(wifi_choice == wifi_target ? "指定网络" : "已保存", 36, 186, 22, 612, 3);
            button(WifiUpdatePassword, "更新密码", 24, 260, 636, 96);
            button(WifiForget, "忘记网络", 24, 380, 636, 96);
            wrapped(notice_text, 36, 1060, 612, 24);
            break;
        case WifiPage::Forget:
            header("忘记网络", 0);
            title_lines("忘记" + quoted_name(wifi_choice) + "？", 36, 160, 612, 34, true, 2);
            wrapped(wifi_choice == wifi_target ? "已保存的密码会删除。连接方式将改回自动连接。" : "已保存的密码会删除。",
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
                // A full list still accepts a new password for a network that it already holds.
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
            title_lines("无法连接" + quoted_name(join_draft.ssid), 36, 160, 612, 34, true, 2);
            const int height = wrapped(join_error + "网络没有保存。", 36, 280, 612, 26);
            const bool password = join_failure == LinkFailure::Password && !join_open;
            if (password) wrapped("密码区分大小写。可点“显示”逐位核对。", 36, 300 + height, 612, 24, true, 3);
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
    /** True when no radio work is running or kept; otherwise wifi_notice names what to end first. */
    bool wifi_free(bool allow_link = false) {
        if (wifi_worker || portal_live || active_network) wifi_notice = "请先结束当前任务。";
        else if (!wifi_enabled()) wifi_notice = "请先打开 Wi-Fi。";
        else if (!allow_link && (network::live(manual_hold) || wifi_online())) wifi_notice = "请先断开 Wi-Fi。";
        else return true;
        return false;
    }
    void start_wifi(WifiOperation::Kind kind, WifiTargets targets, network::Hold& hold) {
        network::release(hold);
        hold = std::make_shared<std::atomic_bool>(false);
        wifi_worker = ++wifi_sequence;
        if (&hold == &manual_hold) manual_target = targets;
        wifi_kind = kind;
        wifi_request = WifiOperation{kind, wifi_worker, std::move(targets), hold};
        wifi_notice.clear();
    }
    void connect_wifi() {
        std::string problem;
        if (wifi_free(true)) {
            auto targets = wifi_targets(problem);
            if (targets.networks.empty()) wifi_notice = problem;
            else start_wifi(WifiOperation::Kind::Connect, std::move(targets), manual_hold);
        }
        draw_wifi(); overlay();
    }
    void scan_wifi() {
        if (wifi_free()) {
            wifi_page = WifiPage::Add;
            wifi_nearby.clear();
            wifi_scanned = false;
            wifi_add_page = 0;
            start_wifi(WifiOperation::Kind::Scan, {}, wifi_hold);
        }
        draw_wifi(); overlay();
    }
    /** Verifies join_draft on the radio; the network is saved only after the join succeeds. */
    void join_wifi() {
        entry.field = TextEntry::Field::None;
        join_failure = LinkFailure::Other;
        wifi_page = WifiPage::JoinFailed;
        if (!provision::valid_wifi(join_draft)) join_error = "网络名称或密码的格式不正确。";
        else if (!wifi_free()) join_error = std::exchange(wifi_notice, std::string());
        else {
            WifiTargets targets;
            targets.networks.push_back(join_draft);
            start_wifi(WifiOperation::Kind::Join, std::move(targets), wifi_hold);
            join_link = true;
            wifi_page = WifiPage::Joining;
        }
        draw_wifi(); overlay();
    }
    /** Ends the add-network flow and drops its password draft. */
    void leave_join(WifiPage page) {
        std::fill(join_draft.password.begin(), join_draft.password.end(), '\0');
        join_draft = {};
        entry = {};
        wifi_page = page;
    }
    void join_complete(const std::string& error, LinkFailure failure) {
        if (!error.empty()) {
            join_error = error; join_failure = failure == LinkFailure::None ? LinkFailure::Other : failure;
            wifi_page = WifiPage::JoinFailed;
            return;
        }
        std::string detail;
        if (!provision::save_network_edit(root, join_draft, false, std::time(nullptr), detail)) {
            join_error = "已连接，但网络没有写入存储卡。请检查 TF 卡。"; join_failure = LinkFailure::Other;
            wifi_page = WifiPage::JoinFailed;
            return;
        }
        refresh_badge_config();
        std::fill(join_draft.password.begin(), join_draft.password.end(), '\0');
        join_draft.password.clear();
        wifi_page = WifiPage::Joined;
    }

    // --- On-screen keyboard ---------------------------------------------------------------
    static constexpr int keyboard_top = 796;
    static constexpr RefreshRegion entry_field_region{0, 104, 684, 320}, entry_keys_region{0, keyboard_top, 684, 1216 - keyboard_top};
    enum class KeyKind { Character, Shift, Backspace, Letters, Digits, Symbols, Enter };
    struct Key {
        std::string label, text;
        KeyKind kind;
        int x, y, w, h;
    };
    bool entry_url() const { return entry.field == TextEntry::Field::Endpoint; }
    bool entry_secret() const {
        return entry.field == TextEntry::Field::WifiPassword || entry.field == TextEntry::Field::Password;
    }
    std::size_t entry_limit() const {
        switch (entry.field) {
        case TextEntry::Field::WifiName: return 32;
        case TextEntry::Field::WifiPassword: return 64;  // 63 characters, or 64 hexadecimal digits.
        case TextEntry::Field::Endpoint: return 1024;
        default: return 256;
        }
    }
    /**
     * A Wi-Fi password is complete as a valid passphrase or key. Empty, for an open network, is
     * accepted only when the network type is unknown: a typed name or a saved network.
     */
    bool entry_ready() const {
        if (entry.field == TextEntry::Field::WifiPassword)
            return entry.value.empty() ? !join_secured
                                       : entry.value.size() >= 8 && provision::valid_wifi({"-", entry.value});
        if (entry.field == TextEntry::Field::WifiName) return !entry.value.empty();
        return true;
    }
    /** Keys of the current layer. Together the three layers hold every printable ASCII character. */
    std::vector<Key> keyboard() const {
        static constexpr const char* rows[3][3] = {{"qwertyuiop", "asdfghjkl", "zxcvbnm"},
                                                   {"1234567890", "-/:;()$&@\"", ".,?!'_%"},
                                                   {"[]{}#^*+=\\", "|~<>`", ""}};
        static constexpr int width = 61, pitch = 67, height = 92, left = 10, first = keyboard_top + 16;
        std::vector<Key> keys;
        const auto characters = [&](const char* values, int x, int y) {
            for (; *values; ++values, x += pitch) {
                const char value = entry.shift && entry.layer == 0 ? static_cast<char>(*values - 32) : *values;
                keys.push_back({std::string(1, value), std::string(1, value), KeyKind::Character, x, y, width, height});
            }
        };
        const auto row = rows[entry.layer];
        characters(row[0], left, first);
        characters(row[1], left + (10 - static_cast<int>(std::strlen(row[1]))) * pitch / 2, first + 100);
        const int third = first + 200, fourth = first + 300;
        if (entry.layer == 0) keys.push_back({entry.shift ? "小写" : "大写", "", KeyKind::Shift, left, third, 94, height});
        else keys.push_back({entry.layer == 1 ? "#+=" : "123", "", entry.layer == 1 ? KeyKind::Symbols : KeyKind::Digits, left, third, 94, height});
        characters(row[2], left + 100, third);
        keys.push_back({"退格", "", KeyKind::Backspace, 580, third, 94, height});
        keys.push_back({entry.layer == 0 ? "123" : "abc", "", entry.layer == 0 ? KeyKind::Digits : KeyKind::Letters, left, fourth, 128, height});
        if (entry_url() && entry.layer == 0) {
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
    void draw_entry() {
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
        // The end of the text and the cursor stay visible; earlier characters scroll out to the left.
        ui.size(34);
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
        button(EntryClear, "清空", 540, 284, 120, 60, false, 24);
        if (entry_secret()) button(EntryMask, entry.masked ? "显示" : "隐藏", 408, 284, 120, 60, false, 24);
        else if (entry_url()) button(EntryDefault, "恢复默认", 380, 284, 148, 60, false, 24);
        text(hint, 36, 372, 24, 612, 3);
        canvas.fill_rect(0, keyboard_top, 684, 3, 1);
        const auto keys = keyboard();
        // Keys carry no controls; key_input() matches touches against the entry model.
        for (const auto& key : keys) {
            const bool enter = key.kind == KeyKind::Enter;
            const int size = key.kind == KeyKind::Character && key.text.size() == 1 && key.text != " " ? 34 : 24;
            if (enter && !entry_ready()) {
                disabled_button(key.label, key.x, key.y, key.w, key.h);
                continue;
            }
            if (enter) canvas.fill_rect(key.x, key.y, key.w, key.h, 1);
            else outline(key.x, key.y, key.w, key.h);
            center(key.label, key.x, key.y + (key.h - size) / 2 - 3, key.w, size, enter ? 15 : 1);
        }
        entry.field_dirty = entry.keys_dirty = false;
        entry.ready_drawn = entry_ready();
    }
    void open_entry(TextEntry::Field field, const std::string& value) {
        entry = {};
        entry.field = field;
        entry.value = value;
        draw_entry(); overlay();
    }
    /**
     * Applies a key to the entry model without touching the canvas, so taps are accepted while a
     * frame is in flight, also when they began before the previous frame finished. Returns false
     * when the touch is not on a key.
     */
    bool key_input(const AppInput& input) {
        if (!input.touch) return false;
        // Keys are matched against the entry model rather than the drawn controls: a tap that
        // follows an undrawn change, such as the enter key after the last required character,
        // meets the keys as they are now. Each key also owns half of the gap around it.
        const auto keys = keyboard();
        const auto inside = [](const Key& key, int x, int y) {
            return x >= key.x - 3 && x < key.x + key.w + 3 && y >= key.y - 4 && y < key.y + key.h + 4;
        };
        const auto found = std::find_if(keys.begin(), keys.end(), [&](const Key& key) { return inside(key, input.x, input.y); });
        if (found == keys.end()) return false;
        // A contact that left the key it started on enters nothing.
        if (input.touch_bounds && (!inside(*found, input.touch_bounds->min_x, input.touch_bounds->min_y) ||
                                   !inside(*found, input.touch_bounds->max_x, input.touch_bounds->max_y))) return true;
        const auto& key = *found;
        switch (key.kind) {
        case KeyKind::Character:
            if (entry.value.size() + key.text.size() > entry_limit()) { entry.field_dirty = true; break; }
            entry.value += key.text;
            entry.field_dirty = true;
            // Shift applies to one letter.
            if (entry.shift) { entry.shift = false; entry.keys_dirty = true; }
            break;
        case KeyKind::Backspace:
            if (entry.value.empty()) break;
            // A saved account name may hold multi-byte characters; remove the whole last one.
            while (entry.value.size() > 1 && (static_cast<unsigned char>(entry.value.back()) & 0xC0) == 0x80) entry.value.pop_back();
            entry.value.pop_back();
            entry.field_dirty = true;
            break;
        case KeyKind::Shift: entry.shift = !entry.shift; entry.keys_dirty = true; break;
        case KeyKind::Letters: entry.layer = 0; entry.keys_dirty = true; break;
        case KeyKind::Digits: entry.layer = 1; entry.shift = false; entry.keys_dirty = true; break;
        case KeyKind::Symbols: entry.layer = 2; entry.keys_dirty = true; break;
        case KeyKind::Enter:
            // Ending the entry draws another page, which needs an idle display.
            if (!display.busy()) entry_done();
            return true;
        }
        // A changed layer has other keys under the same positions; redraw before the next tap is read.
        if (entry.keys_dirty && !display.busy()) flush_entry();
        return true;
    }
    /** Draws every key tap since the last frame in one update of the changed area. */
    void flush_entry() {
        if (view != View::TextEntry || display.busy() || (!entry.field_dirty && !entry.keys_dirty)) return;
        // The enter key follows the text length, so a text change can also repaint the keys.
        const bool field = entry.field_dirty, keys = entry.keys_dirty || entry.ready_drawn != entry_ready();
        draw_entry();
        display.present({{"ui", 0, 0, ++ui_sequence}, {}, FramePurpose::Overlay, RefreshIntent::Content,
                         field && keys ? RefreshRegion{0, 104, 684, 1112} : field ? entry_field_region : entry_keys_region});
    }
    void entry_done() {
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
    // Firmware update: the page under settings, its requests to the network worker and the
    // report of an installation on the next start.
    enum class UpdatePage { Home, Checking, Notes, Schema, Downloading, Failed, Ready, Result };
    /** One laid-out line of release notes. block_rows is the line count of its block on the block's first line, otherwise 0. */
    struct NoteLine {
        std::u32string text;
        std::vector<bool> bold;
        int size = 26, indent = 0, gap = 0;
        bool bullet = false;
        std::size_t block_rows = 0;
    };
    update::State update_state;
    UpdatePage update_page = UpdatePage::Home, notes_return = UpdatePage::Home;
    std::optional<UpdateOperation> update_request;
    network::Hold update_hold;
    // update_active is the request the page waits for; update_worker is the one the worker holds.
    std::uint64_t update_sequence = 0, update_active = 0, update_worker = 0;
    bool update_cancelling = false, update_ready = false, update_rolled_back = false, battery_charging = false;
    // The manifest came from the update server since this start. A cached one is shown, but its
    // address and digest are read from the card and are not used for a download.
    bool update_fresh = false;
    // An installation was requested on a cached manifest; the check in flight continues into it
    // when the server still names the same image.
    bool update_confirming = false;
    UpdateFailure update_failure = UpdateFailure::None;
    std::uint32_t update_bytes = 0;
    int update_step = 0;
    std::string update_notice, update_from, update_to, running_version = firmware_version();
    std::vector<NoteLine> note_lines;
    std::vector<std::size_t> note_starts;
    std::size_t note_page = 0;
    // Text area of a notes screen: later screens start at notes_top, every screen ends at notes_bottom.
    static constexpr int notes_top = 132, notes_first_top = 184, notes_bottom = 984;
    // The state is kept in the rebuildable cache: clearing the cache also forgets a skipped
    // version and an installation result that has not been shown yet.
    void load_update_state() {
        std::string cached;
        update_state = {};
        update_fresh = false;
        if (card_available && store.load_cache(update::state_cache, update::state_limit, cached) == StoreStatus::Ok)
            update::decode_state(cached, update_state);
    }
    bool save_update_state() {
        const auto encoded = update::encode_state(update_state);
        return card_available && !encoded.empty() && store.save_cache(update::state_cache, encoded) == StoreStatus::Ok;
    }
    update::Offer update_offer() const {
        return update_state.manifest ? update::evaluate(*update_state.manifest, running_version, update_state.skipped)
                                     : update::Offer::Current;
    }
    static bool update_installable(update::Offer offer) {
        return offer == update::Offer::Newer || offer == update::Offer::NewerSchema || offer == update::Offer::Development;
    }
    bool update_waiting() const { return update_request || update_worker; }
    /** Why the battery forbids an installation now; empty at 20% or above, or on external power. */
    std::string update_battery_block() const {
        if (battery_charging || (battery_percent && *battery_percent >= 20)) return {};
        return battery_percent ? "电量 " + std::to_string(*battery_percent) + "%。充电或电量到 20% 以上再安装。"
                               : "电量未知。接上电源后再安装。";
    }
    /** Why an installation cannot start now; empty when it can. */
    std::string update_block() const {
        if (const auto battery = update_battery_block(); !battery.empty()) return battery;
        if (active_network || portal_live || wifi_worker || content_loading || foreground_job || save_job || bookmark_job)
            return "请先结束正在进行的同步、传书或书籍加载。";
        return {};
    }
    static int note_line_height(int size) { return size * 31 / 20; }
    /** Wraps and paginates release notes for screens whose first text area starts at first_top. */
    void layout_notes(const std::string& markdown, bool truncated, int first_top) {
        note_lines.clear(); note_starts.clear();
        auto blocks = update::parse_notes(markdown);
        if (blocks.empty()) blocks.push_back({update::NoteBlock::Kind::Text, {{"这一版没有附更新说明。", false}}});
        if (truncated) blocks.push_back({update::NoteBlock::Kind::Text, {{"日志过长，后面的内容未显示。", false}}});
        for (const auto& block : blocks) {
            const bool heading = block.kind == update::NoteBlock::Kind::Heading;
            const bool item = block.kind == update::NoteBlock::Kind::Item;
            NoteLine line{{}, {}, heading ? 30 : 26, item ? 28 : 0, heading ? 28 : 12, item, 0};
            ui.size(line.size);
            std::u32string scalars;
            std::vector<bool> bold;
            for (const auto& span : block.spans)
                for (auto scalar : decode_utf8(span.text)) { scalars.push_back(scalar); bold.push_back(heading || span.bold); }
            const int width = 612 - line.indent;
            const auto first = note_lines.size();
            int used = 0;
            for (std::size_t i = 0; i < scalars.size(); ++i) {
                const int advance = ui.advance(scalars[i]), run = run_width(scalars, i);
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
    /** Draws the lines of screen from top; with bottom set, stops there and marks omitted lines. */
    void draw_note_lines(std::size_t screen, int top, int bottom = 0) {
        const auto begin = note_starts[screen];
        const auto end = screen + 1 < note_starts.size() ? note_starts[screen + 1] : note_lines.size();
        int y = top;
        for (auto i = begin; i < end; ++i) {
            const auto& line = note_lines[i];
            if (i != begin && line.block_rows) y += line.gap;
            if (bottom && y + note_line_height(line.size) > bottom) { text("…", 36, y, 24, 612, 3); return; }
            ui.size(line.size);
            const int baseline = y + ui.metrics().ascender;
            if (line.bullet && line.block_rows) ui.draw(canvas, ui.contains(U'•') ? U'•' : U'·', 36, baseline, 1);
            int x = 36 + line.indent;
            for (std::size_t k = 0; k < line.text.size(); ++k) {
                ui.draw(canvas, line.text[k], x, baseline, 1, line.bold[k]);
                x += ui.advance(line.text[k]);
            }
            y += note_line_height(line.size);
        }
        if (bottom && end < note_lines.size()) text("…", 36, y, 24, 612, 3);
    }
    static std::string update_stage(const std::string& version) {
        return version.find("-beta.") != std::string::npos ? "测试版" : version.find("-rc.") != std::string::npos ? "候选版" : "正式版";
    }
    std::string update_checked() const {
        const auto day = recent_date(update_state.checked_at);
        const auto shifted = static_cast<std::time_t>(update_state.checked_at + 8 * 3600);
        std::tm time{};
        if (day.empty() || !gmtime_r(&shifted, &time)) return "上次检查时间未知";
        char value[8];
        std::strftime(value, sizeof(value), "%H:%M", &time);
        return "上次检查 " + day + " " + value;
    }
    void update_head(const std::string& label, const std::string& version, const std::string& detail = "") {
        text(label, 36, 136, 24, 612, 3);
        text(version, 36, 176, version.size() > 18 ? 32 : 44, 612, 1, true);
        if (!detail.empty()) text(detail, 36, 244, 24, 612, 3);
    }
    /** The three installation steps; steps before active are marked done. */
    void update_steps(int active) {
        static constexpr const char* names[] = {"1 下载", "2 校验", "3 重启"};
        outline(36, 452, 612, 72);
        for (int i = 0; i < 3; ++i) {
            const int x = 36 + i * 204;
            if (i) canvas.line(x, 452, x, 523, 1, 2);
            if (i == active) outline(x, 452, 204, 72, 5);
            center(std::string(names[i]) + (i >= active ? "" : ui.contains(U'✓') ? " ✓" : " 完成"), x, 474, 204, 24,
                   i == active ? 1 : 3, i == active);
        }
    }
    /** A bottom panel over the page already on the canvas; returns the top of its body. Only the panel's controls stay active. */
    int update_sheet(int top, const std::string& title) {
        hits.clear();
        canvas.fill_rect(0, top, 684, 1216 - top, 15);
        canvas.line(0, top, 683, top, 1, 3);
        canvas.fill_rect(36, top + 36, 48, 48, 1);
        center("!", 36, top + 42, 48, 32, 15, true);
        text(title, 100, top + 42, 32, 548, 1, true);
        return top + 112;
    }
    void update_install_button(int y, update::Offer offer) {
        const auto label = offer == update::Offer::Development ? "换成发布构建" : "下载并安装";
        if (update_installable(offer) && update_block().empty())
            button(UpdateStart, label, 24, y, 636, 96, offer != update::Offer::Development);
        else {
            disabled_button(label, 24, y, 636, 96);
            // Battery and activity change without a redraw; a tap evaluates them again.
            if (update_installable(offer)) hits.push_back({24, y, 636, 96, UpdateStart});
        }
    }
    /** preview_bottom limits the release-note excerpt; a panel drawn over the page passes its own top. */
    void draw_update_home(update::Offer offer, int preview_bottom = 700) {
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
        if (!manifest || offer == update::Offer::Current || offer == update::Offer::Skipped) {
            update_head(development ? "当前版本 · 开发构建" : "当前版本", running_version);
            canvas.line(36, 300, 647, 300, 1, 2);
            text(!manifest ? "尚未检查更新" : offer == update::Offer::Skipped ? "没有可安装的更新" : "已是最新版本", 36, 328, 30, 612, 1, true);
            int y = 384;
            if (manifest) { text(update_checked(), 36, y, 24, 612, 3); y += 52; }
            if (offer == update::Offer::Skipped)
                y += wrapped(manifest->version + " 上次没有启动成功，已跳过。清单出现更高的版本后可以更新。", 36, y, 612, 26) + 24;
            if (manifest && manifest->version == running_version) value_row(UpdateNotes, "本版更新日志", "", "", y, 96);
            wrapped(update_notice, 36, 940, 612, 24);
            check_button(1088, "检查更新");
            return;
        }
        if (note_lines.empty()) layout_notes(manifest->notes, manifest->notes_truncated, notes_first_top);
        if (offer == update::Offer::Development) {
            update_head("当前版本 · 开发构建", running_version);
            canvas.line(36, 300, 647, 300, 1, 2);
            text("清单版本", 36, 324, 24, 612, 3);
            text(manifest->version, 36, 362, 36, 612, 1, true);
            wrapped("开发构建无法判断新旧。安装后会换成清单里的发布构建，本机改动不保留。", 36, 432, 612, 26);
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
        const auto reason = offer == update::Offer::TooLarge ? "新固件 " + file_size(manifest->size) + "，超过 4 MiB 的分区，不能安装。"
            : offer == update::Offer::OlderSchema ? std::string("这个版本写入的数据格式比本机旧，不能安装。") : update_block();
        wrapped(update_notice.empty() ? reason : update_notice, 36, 1104, 612, 24);
    }
    void draw_update() {
        view = View::Update;
        const auto& manifest = update_state.manifest;
        const auto offer = update_offer();
        if (update_page == UpdatePage::Result && update_rolled_back) {
            int y = update_sheet(700, "更新没有完成");
            y += wrapped(update_to + " 没有启动成功，已回到 " + running_version + "。阅读进度没有变化。", 36, y, 612, 26) + 16;
            wrapped("设备会跳过这个版本，直到清单出现更高的版本。", 36, y, 612, 24, true, 3);
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
        case UpdatePage::Checking:
            header("固件更新", 0);
            update_head("当前版本", running_version);
            canvas.line(36, 300, 647, 300, 1, 2);
            text(update_cancelling ? "正在取消" : "正在检查", 36, 328, 30, 612, 1, true);
            text("网络：" + network_label() + " · 读取更新清单", 36, 384, 24, 612, 3);
            disabled_button(update_cancelling ? "正在取消…" : "正在检查…", 24, 976, 636, 96);
            if (!update_cancelling) button(UpdateCancel, "取消", 24, 1088, 636, 88);
            return;
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
            case UpdateFailure::Link: title = "没有连上网络"; body = update_notice; break;
            case UpdateFailure::Network: title = "网络中断"; body = "下载到 " + megabytes(update_bytes) + " MiB 时网络断开。"; break;
            case UpdateFailure::Server: title = "更新服务器无响应"; body = "没有取得固件文件，稍后再试。"; break;
            case UpdateFailure::Mismatch: title = "校验没有通过"; body = "下载的文件和清单记录的不一致，已丢弃。"; break;
            case UpdateFailure::TooLarge: title = "空间不够"; body = "新固件 " + file_size(manifest->size) + "，超过 4 MiB 的分区。"; break;
            case UpdateFailure::WrongImage: title = "型号不符"; body = "这个固件不是给 Read Pico 的，或版本与清单不符。"; break;
            default: break;
            }
            canvas.fill_rect(36, 140, 48, 48, 1);
            center("!", 36, 146, 48, 32, 15, true);
            text(title, 100, 144, 34, 548, 1, true);
            wrapped(body + "仍在使用 " + running_version + "。", 36, 228, 612, 26);
            button(UpdateRetry, "重新下载", 24, 976, 636, 96, true);
            button(UpdateLater, "稍后", 24, 1088, 636, 88);
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
    /** Queues a check or a download for the network worker, or shows why it cannot start. */
    void update_begin(UpdateOperation::Kind kind) {
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
    /** Ends a request the worker has not taken; otherwise asks the worker to stop and waits for its completion. */
    void update_cancel() {
        network::release(update_hold);
        update_confirming = false;
        if (update_request) {
            update_request.reset();
            update_active = 0;
            update_page = UpdatePage::Home;
        } else if (update_worker) update_cancelling = true;
    }
    void update_progress(std::uint64_t id, std::uint32_t bytes) {
        if (!id || id != update_worker || id != update_active || update_cancelling || display.busy() ||
            view != View::Update || update_page != UpdatePage::Downloading || !update_state.manifest) return;
        const auto total = update_state.manifest->size;
        // Twenty 5% steps while downloading; the last value marks the verification that follows.
        const int step = bytes >= total ? 21 : static_cast<int>(20ull * bytes / total);
        if (step == update_step) return;
        update_step = step;
        update_bytes = bytes;
        draw_update();
        display.present({{"ui", 0, 0, ++ui_sequence}, {}, FramePurpose::Overlay, RefreshIntent::Content,
                         step == 21 ? std::nullopt : std::optional<RefreshRegion>(RefreshRegion{0, 300, 684, 124})});
    }
    void update_complete(std::uint64_t id, UpdateFailure failure, int status, const std::string& body, const std::string& detail) {
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
            if (failure == UpdateFailure::Link) update_notice = detail;
            // The update host answers 404 until the first manifest is published.
            else if (failure == UpdateFailure::None && status == 404) update_notice = "更新服务器上还没有发布固件。";
            else if (failure != UpdateFailure::None || status != 200 || !update::parse_manifest(body, manifest, problem))
                update_notice = "更新服务器无响应，稍后再试。";
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
                if (confirming && !same) update_notice = "更新清单有变化，请确认后再安装。";
                const auto offer = update_offer();
                if (confirming && same && update_installable(offer) && update_block().empty()) {
                    if (offer != update::Offer::NewerSchema) { update_begin(UpdateOperation::Kind::Download); return; }
                    update_page = UpdatePage::Schema;
                }
            }
        }
        if (!display.busy()) { draw_update(); overlay(); }
    }
    /** Shows the outcome of an installation that restarted the device; the library is already drawn. */
    void update_boot() {
        load_update_state();
        if (update_state.pending_to.empty()) return;
        update_from = update_state.pending_from;
        update_to = update_state.pending_to;
        update_rolled_back = running_version != update_to;
        update_page = UpdatePage::Result;
        note_lines.clear(); note_page = 0;
        draw_update();
    }
    /** Opens the update page from settings with the cached state, or returns to work in progress. */
    void update_open() {
        if (view != View::Settings || !bookmark_io_ready()) return;
        message.clear(); update_notice.clear();
        if (!update_waiting()) {
            // A manifest fetched since this start stays; the card is read only for one that is not.
            if (!update_ready && !update_fresh) load_update_state();
            update_page = UpdatePage::Home;
            note_lines.clear(); note_page = 0;
        }
        draw_update(); overlay();
    }
    /** Handles a control of the update page; false leaves id to the common controls. */
    bool update_control(int id) {
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
                update_begin(UpdateOperation::Kind::Check);
                update_confirming = update_waiting();
                return true;
            }
            if (offer != update::Offer::NewerSchema) { update_begin(UpdateOperation::Kind::Download); return true; }
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
    /** The single frame shown while the device restarts into verified firmware; ends input like the power-off page. */
    void draw_installing() {
        view = View::PowerOff;
        hits.clear(); canvas.clear();
        text("正在安装", 72, 430, 24, 540, 3);
        text(update_to, 72, 474, update_to.size() > 18 ? 34 : 48, 540, 1, true);
        canvas.line(72, 566, 611, 566, 1, 3);
        text("请勿断电", 72, 598, 34, 540, 1, true);
        wrapped("屏幕会停在这一页，直到新固件画出首屏。如果安装没有完成，设备会自动回到原来的版本。", 72, 664, 540, 26);
    }
    void draw_settings() {
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
        row(Import, "网络与传输", "Wi-Fi · " + wifi_summary());
        row(SyncSettings, "KOReader 同步", configured ? server_name(account) + " · " + account.username : "未配置");
        row(Refresh, "显示与刷新", refresh_name(values) + " · " +
            (values.refresh_preset == "clear" ? "每页全刷" : "每 " + std::to_string(refresh_interval(values)) + " 页全刷"));
        row(InputSettings, "输入与阅读", std::string(values.library_view == "grid" ? "卡片" : "列表") + "视图 · " + typography_summary(values));
        row(PowerSettings, "电源与休眠", sleep_name(values) + " · " +
            (values.auto_sleep_minutes ? std::to_string(values.auto_sleep_minutes) + " 分钟" : "不自动休眠"));
        row(StorageSettings, "存储与设备", (measured ? "可用 " + file_size(card_free) : "可用空间未知") + " · 固件 " + firmware_version());
        const auto offer = update_offer();
        entry_row(UpdateOpen, "固件更新", "", offer == update::Offer::Newer || offer == update::Offer::NewerSchema
                  ? "新版本 " + update_state.manifest->version : running_version, y, 80);
        entry_row(Help, "使用说明", "", "离线查看", y + 80, 80);
        entry_row(About, "关于", "", "", y + 160, 80);
        if (session.needs_save()) {
            // Unsaved progress outranks other notices; the whole footer retries the save.
            footer_bar("进度尚未保存 · 点此重试");
            hits.push_back({0, footer_zone, 684, 96, SaveRetry});
        } else footer_bar(cfg.diagnostics.empty() ? notice() : "设置文件无效，请检查后重新打开。");
    }
    void draw_about() {
        view = View::About;
        hits.clear(); canvas.clear(); header("关于", Back, "设置");
        text("Pico Page", 36, 144, 44, 612, 1, true);
        text("Read Pico 墨水屏阅读固件", 36, 206, 22, 612, 3);
        canvas.line(24, 256, 660, 256, 1, 2);
        value_row(0, "版本", "", firmware_version(), 256, 70);
        value_row(0, "硬件", "", hardware_name(), 326, 70);
        value_row(0, "作者", "", "wulu", 396, 70);
        value_row(0, "项目主页", "", kProjectHome, 466, 70);
        graphics::draw_qr(canvas, std::string("https://") + kProjectHome, 36, 572, 224);
        text("扫码打开项目主页", 284, 594, 28, 364, 1, true);
        wrapped("查看介绍和字体下载。", 284, 642, 364, 22, true, 3);
        text("第三方组件", 36, 840, 22, 612, 3);
        canvas.line(24, 876, 660, 876, 1, 2);
        value_row(0, "epdiy 显示驱动", "", "LGPL-3.0-or-later", 876, 64);
        value_row(0, "内置正文字体", "", "OFL 1.1", 940, 64);
        text("源码整理中，敬请期待。", 36, 1036, 22, 612, 3);
        footer_bar(notice());
    }
    void help_regions(int x, int y, int width, int height) {
        const int left = width * 205 / 684, right = width * 479 / 684, foot = height * footer_top / 1216;
        const int device = width * reading_footer::device_left / 684;
        const std::array<Hit, 5> zones{{{x, y, left, foot, HelpPrevious},
            {x + left, y, right - left, foot, HelpTools}, {x + right, y, width - right, foot, HelpNext},
            {x, y + foot, device, height - foot, HelpPosition}, {x + device, y + foot, width - device, height - foot, HelpDevice}}};
        for (const auto& zone : zones) {
            canvas.fill_rect(zone.x, zone.y, zone.w, zone.h, zone.id == help_zone ? 1 : 15);
            hits.push_back(zone);
        }
        for (int split : {left, right}) canvas.line(x + split, y, x + split, y + foot, 9, 2);
        canvas.line(x, y + foot, x + width, y + foot, 9, 2);
        canvas.line(x + device, y + foot, x + device, y + height, 9, 2);
        outline(x, y, width, height);
    }
    void draw_help_demo() {
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
            const char* positions[] = {"第 12 / 340 页", "3.53%", "第 2 / 10 章", "第一章 从一页开始"};
            text(positions[help_position], 36, 1154, 24, 476, help_zone == HelpPosition ? 15 : 1);
            center(help_device ? "12:00" : "84%", 540, 1154, 144, 24, help_zone == HelpDevice ? 15 : 1);
            return;
        }
        help_regions(36, 276, 274, 486);
        const char* labels[] = {"左侧：上一页", "中部：打开工具", "右侧：下一页", "页脚左：切换位置", "页脚右：电量 / 时钟"};
        for (int i = 0; i < 5; ++i) {
            const int id = HelpPrevious + i, y = 290 + i * 88;
            if (help_zone == id) canvas.fill_rect(330, y, 330, 72, 1);
            text(labels[i], 342, y + 22, 24, 306, help_zone == id ? 15 : 1);
            hits.push_back({330, y, 330, 72, id});
        }
        text("点示意图或说明，查看对应区域", 36, 786, 24, 612, 3);
        for (int i = 0; i < 3; ++i)
            button(HelpKey1 + i, "K" + std::to_string(i + 1), 24 + i * 216, 842, 204, 72, help_key == HelpKey1 + i, 26);
        const char* description = help_key == HelpKey2 ? "所有可交互页面：立即全刷。" :
            help_key == HelpKey1 ? "正文：上一页。书库、目录、说明：上一屏。" : "正文：下一页。书库、目录、说明：下一屏。";
        wrapped(description, 36, 942, 612, 24);
        text(help_key == HelpKey2 ? "不关闭面板，不改变阅读位置。" : "工具、排版和键盘中不翻动正文。", 36, 1030, 24, 612, 3);
        footer_bar("", help_page, kHelpPages.size());
    }
    void draw_help() {
        view = View::Help;
        hits.clear(); canvas.clear(); header("使用说明", Back, help_from_settings ? "设置" : "书库");
        const auto& page = kHelpPages[help_page];
        if (help_page == 3) { draw_help_demo(); return; }
        text(page.title, 36, 144, 34, 612, 1, true);
        text("Pico Page · " + firmware_version(), 36, 204, 22, 612, 3);
        if (help_page == 2) { draw_help_demo(); return; }
        int y = 276;
        for (const auto& section : page.sections) {
            text(section.title, 36, y, 28, 612, 1, true);
            y += 48;
            y += wrapped(section.body, 36, y, 612, 26);
            y += 36;
        }
        if (help_page == 1) text("下一屏可直接点按试一试", 36, 1058, 24, 612, 3);
        footer_bar("", help_page, kHelpPages.size());
    }
    std::vector<std::string> lock_screens;
    std::size_t lock_screen_page = 0;
    std::string lock_screen_error, custom_notice;
    std::chrono::steady_clock::time_point custom_retry_at{};
    bool clear_cache_pending = false;
    bool custom_prepare_finished = false, custom_sleep_attempted = false, custom_notice_pending = false;
    std::unique_ptr<graphics::Canvas> custom_sleep_image;
    void list_lock_screens() {
        abandon_content();
        if (counting_job) counting_job->cancel();
        if (cover_job) cover_job->cancel();
        ContentJob::Request request;
        request.kind = ContentJob::Kind::LockScreenList;
        request.root = root; request.generation = foreground_generation;
        pending_content = std::move(request);
        loading("正在读取休眠图片");
    }
    void draw_lock_screens() {
        view = View::LockScreens;
        hits.clear(); canvas.clear(); header("自定义休眠图片", Back, "休眠");
        choice(SleepRotation, "按文件名轮播", "每次休眠使用下一张图片", 140, draft.sleep_image.empty(),
               draft.sleep_image != edit_snapshot.config.settings.sleep_image || draft.sleep_screen != edit_snapshot.config.settings.sleep_screen);
        text("LockScreens/ · PNG / JPEG", 36, 280, 24, 612, 3);
        const auto pages = std::max<std::size_t>(1, (lock_screens.size() + 5) / 6);
        lock_screen_page = std::min(lock_screen_page, pages - 1);
        for (std::size_t i = lock_screen_page * 6; i < lock_screens.size() && i < (lock_screen_page + 1) * 6; ++i) {
            const int y = 334 + static_cast<int>(i % 6) * 102;
            button(LockScreenBase + static_cast<int>(i), lock_screens[i], 36, y, 612, 88,
                   draft.sleep_image == lock_screens[i], 24);
        }
        if (lock_screens.empty()) note(lock_screen_error.empty() ? "请将图片放入 TF 卡 LockScreens/ 目录。" : lock_screen_error, 380);
        button(SleepRefresh, "重新读取", 36, 980, 280, 88);
        button(Back, "完成", 340, 980, 308, 88, true);
        footer_bar("返回后保存设置", lock_screen_page, pages);
    }
    /** Requests cancellation and reports whether a custom image worker still owns storage. */
    bool cancel_custom_preparation() {
        if (!counting_job || counting_job->request.kind != ContentJob::Kind::LockScreen || counting_job->done()) return false;
        counting_job->cancel();
        return true;
    }
    bool custom_prepare_due() const {
        return settings.sleep_screen == "custom" && !custom_prepare_finished &&
               std::chrono::steady_clock::now() >= custom_retry_at && card_available && !usb_prepared &&
               !portal_live && !portal_requested && usb_action != UsbTransferAction::Start &&
               !sleep_cover_deadline && !power_menu_wait && !active_network && !network_worker_id &&
               !display.busy() && !content_loading && !foreground_job && !pending_content && !counting_job &&
               !body_images_pending && !inline_image_pending && !session.needs_save() && !save_job && !bookmark_job && !cover_job &&
               (view == View::Library || view == View::Reading || view == View::Settings || view == View::PowerSettings);
    }
    void prepare_custom_sleep(bool sleeping) {
        ContentJob::Request request;
        request.kind = ContentJob::Kind::LockScreen; request.root = root;
        request.sleep_image = settings.sleep_image; request.generation = counting_generation;
        request.lock_screen_prepare = !sleeping; request.preview = sleeping;
        if (!sleeping) custom_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        if (sleeping) { custom_sleep_attempted = true; custom_sleep_image.reset(); custom_notice.clear(); }
        counting_job = std::make_unique<ContentJob>(std::move(request));
        if (!counting_job->start()) {
            counting_job.reset();
            if (sleeping) custom_notice = "休眠图片任务无法启动";
        }
    }
    void draw_power() {
        view = View::PowerSettings;
        hits.clear(); canvas.clear(); header("电源与休眠", Back, "设置");
        text("休眠画面", 36, 144, 24, 612, 3);
        button(SleepCover, "书籍封面", 36, 192, 296, 92, draft.sleep_screen == "cover");
        button(SleepReading, "保留正文", 352, 192, 296, 92, draft.sleep_screen == "reading");
        button(SleepSimple, "简洁屏保", 36, 308, 296, 92, draft.sleep_screen == "simple");
        button(SleepCustom, "自定义图片", 352, 308, 296, 92, draft.sleep_screen == "custom");
        if (draft.sleep_screen == "custom") {
            button(SleepChoose, "选择图片 / 轮播", 36, 432, 612, 88);
            text(draft.sleep_image.empty() ? "按文件名轮播" : draft.sleep_image, 36, 548, 24, 612, 3);
        } else if (draft.sleep_screen == "cover") note("无可用封面时显示书名与作者。", 464);
        else if (draft.sleep_screen == "reading") note("保留当前正文，并显示休眠提示。", 464);
        else note("显示书名、阅读位置与唤醒提示。", 464);
        text("自动休眠", 36, 636, 28, 612, 1, true);
        button(SleepLess, "−", 36, 706, 120, 96);
        center(draft.auto_sleep_minutes ? std::to_string(draft.auto_sleep_minutes) + " 分钟" : "关闭", 168, 728, 348, 34);
        button(SleepMore, "+", 528, 706, 120, 96);
        note("按电源键唤醒；保存失败时不进入休眠。", 856);
        button(PowerMenu, "关机 / 重启", 24, 952, 636, 96);
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
    static const char *hardware_name() {
#ifdef ESP_PLATFORM
        return "Read Pico";
#else
        return "桌面预览";
#endif
    }
    static std::string firmware_version() {
#ifdef ESP_PLATFORM
        return esp_app_get_description()->version;
#else
        return PICO_PAGE_VERSION;
#endif
    }
    /**
     * Sums regular files below a card-relative directory; books_only keeps EPUB and TXT names.
     * The walk blocks the application task, so it stops after 2000 entries. nullopt when a
     * directory cannot be read or that limit is exceeded.
     */
    std::optional<std::uint64_t> tree_bytes(const std::string &relative, bool books_only) {
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
                else if (S_ISREG(st.st_mode) && (!books_only || book_name(name))) total += static_cast<std::uint64_t>(st.st_size);
            }
            closedir(dir);
        }
        return total;
    }
    void draw_storage() {
        if (!bookmark_io_ready()) return;
        view = View::StorageSettings;
        hits.clear(); canvas.clear(); header("存储与设备", Back, "设置");
        std::optional<std::uint64_t> books = 0;
        const auto roots = store.load_settings().settings.library_roots;
        for (const auto &path : roots) {
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
        value_row(0, "硬件", "", hardware_name(), 508, 70);
        text("文件位置", 36, 606, 22, 612, 3);
        value_row(0, "书籍与文章", "", roots.size() == 1 ? roots.front() + "/" : "多个目录（见设置文件）", 642, 70);
        value_row(0, "自行添加的字体", "", "Fonts/", 712, 70);
        value_row(0, "设备设置", "", "PicoPage/settings.toml", 782, 70);
        value_row(0, "阅读进度", "", "原书旁的 .picopage/", 852, 70);
        note(notice().empty() ? "文章可放扫描目录下的 Articles/；保留 PicoPage/。" : message, 950);
        note("清理缓存不会删除书和阅读进度，下次打开会稍慢。", 1020);
        button(ClearCache, "清理全部缓存", 24, 1080, 636, 96);
    }
    void draw_input_settings() {
        if (!bookmark_io_ready()) return;
        view = View::InputSettings;
        hits.clear(); canvas.clear(); header("输入与阅读", Back, "设置");
        const auto values = store.load_settings().settings;
        value_row(LibraryView, "书库默认视图", "点按切换", values.library_view == "grid" ? "卡片" : "列表", 104, 104, false, false);
        value_row(has_book() ? Typography : 0, "阅读默认排版", typography_summary(values), "", 208);
        text("按键与触摸", 36, 344, 22, 612, 3);
        note("K1 上一页，K3 下一页。", 388);
        note("K2 在所有页面全刷当前画面，不改变位置。", 440);
        note("触摸正文左右区域翻页，中部打开工具。", 492);
        note("长按书库中的书，查看书籍详情。", 544);
        if (!has_book()) note("打开一本书后，可在排版页调整阅读默认排版。", 596);
        button(FullRefresh, "立即全刷", 24, 680, 636, 96);
        value_row(LibraryFolders, "书库文件夹", "最近阅读排序时生效", values.library_folders == "first" ? "置顶" : "跟随最近", 800, 104, false, false);
        if (!notice().empty()) {
            canvas.line(0, 1012, 683, 1012, 1, 3);
            wrapped(message, 36, 1044, 612, 22);
        }
    }
    /** Loads the saved account into the draft; called when the page is opened from another page. */
    void open_account() {
        sync::Config saved;
        std::string detail;
        std::fill(account.password.begin(), account.password.end(), '\0');
        account = {};
        account.saved = sync::load_config(root, saved, detail);
        account.endpoint = account.saved ? saved.endpoint : provision::default_endpoint;
        account.username = saved.username;
        account_page = AccountPage::Form;
        account_notice.clear();
    }
    /** The draft as a configuration; false when a field is missing or malformed. */
    bool account_config(sync::Config& next) const {
        sync::Config saved;
        std::string detail;
        const bool had = sync::load_config(root, saved, detail);
        next = had ? saved : sync::Config{};
        next.endpoint = account.endpoint;
        next.username = account.username;
        // An unchanged password keeps the stored key; the plain password is never stored.
        if (!account.password.empty()) next.userkey = md5_hex(account.password);
        else if (!had || saved.username != account.username) next.userkey.clear();
        if (next.device_id.empty()) next.device_id = writer;
        return sync::valid_config(next);
    }
    void draw_sync_settings() {
        if (!bookmark_io_ready()) return;
        view = View::SyncSettings;
        hits.clear(); canvas.clear();
        if (account_page == AccountPage::Remove) {
            header("移除同步账户", 0);
            text("移除同步账户？", 36, 160, 34, 612, 1, true);
            wrapped("设备会删除 PicoPage/sync.toml。阅读位置仍保存在存储卡，服务器上的记录不受影响。", 36, 244, 612, 28);
            button(Back, "取消", 24, 1080, 306, 96, true);
            button(AccountRemoveConfirm, "移除", 354, 1080, 306, 96);
            return;
        }
        if (account_page == AccountPage::Verifying) {
            header("KOReader 同步", 0);
            text("正在验证账户", 36, 160, 34, 612, 1, true);
            text("网络：" + network_label(), 36, 236, 24, 612, 3);
            button(AccountCancel, "取消", 24, 1000, 636, 96);
            note("取消后不保存账户。", 1124);
            return;
        }
        header("KOReader 同步", Back, "设置");
        sync::Config next;
        const bool complete = account_config(next);
        note(!account.saved || account.changed ? "账户 · 未保存"
             : account.check == AccountDraft::Check::Rejected ? "账户 · 验证未通过"
             : account.check == AccountDraft::Check::Accepted ? "账户 · 已验证" : "账户 · 已保存", 130);
        value_row(AccountEndpoint, "服务地址", "", server_name(next), 170, 96);
        value_row(AccountUser, "账户", "", account.username.empty() ? "未填写" : account.username, 266, 96);
        value_row(AccountPassword, "密码", "", !account.password.empty() ? "已填写" : !next.userkey.empty() ? "已保存" : "未填写", 362, 96);
        wrapped("验证时需要联网。也可以在电脑上编辑 PicoPage/sync.toml。", 36, 482, 612, 22, true, 3);
        const auto verify = account.saved && !account.changed ? "重新验证" : "验证并保存账户";
        if (complete) button(AccountVerify, verify, 24, 580, 636, 96, account.changed || !account.saved);
        else disabled_button(verify, 24, 580, 636, 96);
        if (account.saved) button(AccountRemove, "移除同步账户", 24, 700, 636, 96);
        if (has_book() && account.saved && !account.changed && !txt_name(book_path))
            value_row(Sync, "检查《" + document.metadata().title + "》的同步", "", "", 828, 96);
        else if (has_book() && txt_name(book_path)) note("TXT 只保存本机进度，不支持 KOReader 接续。", 860);
        wrapped(account_notice, 36, 1040, 612, 24);
    }
    void verify_account() {
        sync::Config next;
        sync::HttpRequest request;
        std::string problem;
        if (!account_config(next) || !sync::auth_request(next, request, problem)) {
            account_notice = "服务地址必须以 https:// 开头，账户和密码不能为空。";
        } else if (!wifi_enabled()) account_notice = "Wi-Fi 已关闭。请先在 Wi-Fi 设置中打开。";
        else if (wifi_worker || portal_live || active_network) account_notice = "请先结束当前任务。";
        else {
            auto targets = wifi_targets(problem);
            if (targets.networks.empty()) account_notice = problem;
            else {
                account_check = next;
                account_hold = std::make_shared<std::atomic_bool>(false);
                active_network = ++network_sequence;
                uploading = false;
                network_cancel = std::make_shared<std::atomic_bool>(false);
                network_request = NetworkOperation{active_network, std::move(request), network_cancel, account_hold, std::move(targets)};
                account_page = AccountPage::Verifying;
                account_notice.clear();
            }
        }
        draw_sync_settings(); overlay();
    }
    /** Stops an account check; a late completion is discarded by its request ID. */
    void cancel_account() {
        if (!account_check) return;
        network::release(account_hold);
        if (network_cancel) network_cancel->store(true);
        active_network = 0;
        network_request.reset();
        account_check.reset();
        account_page = AccountPage::Form;
    }
    /** Saves the checked account only after the server accepted it. */
    void account_complete(int status, const std::string& failure, LinkFailure link) {
        const auto checked = *std::exchange(account_check, std::nullopt);
        network::release(account_hold);
        active_network = 0;
        account_page = AccountPage::Form;
        std::string detail;
        if (!failure.empty()) account_notice = link != LinkFailure::None ? failure + "账户没有保存。" : "服务器没有响应，或网络中断。账户没有保存。";
        else if (status == 401 || status == 403) {
            account.check = AccountDraft::Check::Rejected;
            account_notice = account.saved && !account.changed ? "服务器拒绝了已保存的账户，账户或密码不正确。"
                                                               : "账户或密码不正确。账户没有保存。";
        }
        else if (status != 200) account_notice = "同步服务返回 HTTP " + std::to_string(status) + "，无法确认账户。账户没有保存。";
        else if (store.save_private("sync.toml", provision::sync_toml(checked), detail) != StoreStatus::Ok)
            account_notice = "账户已通过验证，但没有写入存储卡。请检查 TF 卡。";
        else {
            open_account();
            account.check = AccountDraft::Check::Accepted;
            refresh_badge_config();
            account_notice = "账户已验证并保存。";
        }
        if (view == View::SyncSettings && !display.busy()) { draw_sync_settings(); overlay(); }
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
        detail_path = path; detail_metadata = {}; detail_progress = {}; detail_cover.reset();
        detail_position.clear(); detail_error.clear(); detail_size.reset();
        if (async_content && safe_existing(root, path)) {
            abandon_content(); invalidate_counting(); release_counting();
            begin_open(path, ContentJob::Kind::Inspect); return;
        }
        struct stat st{};
        if (!safe_existing(root, path) || stat((root + "/" + path).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            detail_error = "原文件不可用，请检查 TF 卡。";
        } else {
            detail_size = st.st_size;
            EpubDocument inspected;
            if (inspected.open(root + "/" + path, detail_error)) {
                const auto *source = &inspected;
                detail_metadata = source->metadata();
                detail_progress = store.load_progress(path, detail_metadata.book_id, revision_for(path, detail_metadata));
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
        if (async_content) { if (detail_cover) cover = detail_cover->pixels(); }
        else if (detail_error.empty() && stat((root + "/" + detail_path).c_str(), &info) == 0)
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
        text(txt_name(detail_path) ? "仅本地" : detail_path == book_path && detail_metadata.book_id == document.metadata().book_id ? sync_badge() : "打开书籍后检查", 186, 478, 24, 462);
        text("原书占用", 36, 548, 24, 150, 3);
        text(detail_size ? file_size(*detail_size) : "未知", 210, 548, 24, 438);
        if (detail_error.empty()) button(DetailOpen, detail_progress.record ? "继续阅读" : "打开书籍", 36, 630, 612, 96, true);
        else wrapped("无法打开：" + reason(detail_error, "无法解析文件内容") + "。", 36, 620, 612, 24);
        canvas.line(0, 764, 683, 764, 1, 3);
        button(DetailCache, "清理本书缓存", 36, 788, 612, 76);
        if (detail_error.empty()) button(DetailReset, "清除本地阅读进度", 36, 880, 612, 76);
        button(DetailDelete, "删除原书文件", 36, 972, 612, 76);
        footer_bar(notice());
    }
    /**
     * Confirmation page: the header names the origin, followed by the question, the affected
     * object when there is one, and the consequence. The caller adds the two buttons at 1080;
     * the filled one is the recommended choice.
     */
    void confirm_page(const std::string &origin, const std::string &question, const std::string &object,
                      const std::string &path, const std::string &consequence) {
        hits.clear(); canvas.clear(); header(origin, 0);
        text(question, 36, 160, 40, 612, 1, true);
        int y = 244;
        if (!object.empty()) {
            title_lines(object, 36, y, 612, 30);
            y += std::min(wrapped(object, 0, 0, 612, 30, false), 80) + 4;
            title_lines(path, 36, y, 612, 22, false, 2, 3);
            y += std::min(wrapped(path, 0, 0, 612, 22, false), 64) + 28;
        }
        wrapped(consequence, 36, y, 612, 28);
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
        confirm_page("书籍管理", remove ? "删除这本书？" : "清除本地进度？",
                     detail_metadata.title.empty() ? basename(detail_path) : detail_metadata.title, detail_path,
                     remove ? "将删除 TF 卡中的原书文件。阅读记录和排版设置会保留，放回同一本书后可以恢复。服务器上的记录不受影响。"
                            : "将清除本书在本机的阅读位置及其备份，下次打开从头开始。原书和排版设置会保留。服务器上的记录不受影响。");
        button(Back, "取消", 24, 1080, 306, 96, true);
        danger_button(ManageConfirm, remove ? "删除原书" : "清除进度", 354, 1080, 306);
    }
    void manage_book() {
        if (!bookmark_io_ready()) return;
        if (detail_path == book_path) {
            cancel_sync();
            session.open("", "", writer);
            document.close(); book_path.clear(); release(chapter); page = {};
            links.clear(); clear_page_history(); link_return.reset(); confirmed_link_return.reset();
            last_recorded.clear();
        }
        abandon_content(); foreground_job.reset(); stop_covers(); clear_prefetch();
        stop_counting_job();
        identities.clear();
        font_identity.clear(); font_scan.reset(); effective_font.clear();
        const auto result = store.manage_book(management_snapshot, delete_original, detail_metadata.book_id, revision_for(detail_path, detail_metadata));
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
            // The recorded percentage belongs to the cleared position.
            store.save_cache(position_name(detail_path), "");
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
        const char *resume_hint = resume_to_library ? "按电源键返回书库" : "按电源键继续阅读";
        if (settings.sleep_screen == "custom" && custom_sleep_image) {
            canvas = *custom_sleep_image;
            custom_sleep_image.reset();
            view = View::Sleeping;
            return;
        }
        if (settings.sleep_screen == "simple" || settings.sleep_screen == "custom") {
            canvas.clear();
            text("已休眠", 72, 440, 40, 540, 1, true);
            text(resume_hint, 72, 512, 26, 540);
            canvas.line(72, 594, 612, 594, 1, 2);
            cover_title(has_book() ? document.metadata().title : "Pico Page", 72, 626, 540, 140, 32);
            text(has_book() ? chapter_label() + " · " + position : "尚未打开书籍", 72, 806, 22, 540, 3);
            text(has_book() ? "已保存 · " + sync_state : "", 72, 854, 22, 540, 3);
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
                if (safe_existing(root, book_path) && stat((root + "/" + book_path).c_str(), &info) == 0) {
                    if (async_content) cached_cover(cover_entry(book_path, info, 684, 1040, true), cover);
                    else cover = cover_pixels(book_path, info, 684, 1040, true);
                }
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
        text(resume_hint, 36, top + 72, 22, 290, 15);
        text(has_book() ? "已保存" : "尚未打开书籍", 364, top + 24, 22, 284, 15);
        text(has_book() ? sync_state : "", 364, top + 60, 22, 284, 15);
        if (!reading && has_book()) text(position, 364, top + 106, 22, 284, 15);
        view = View::Sleeping;
    }
    /** A row of the network page: 30 px title, optional 24 px subtitle and a value with a chevron. */
    void entry_row(int id, const std::string &title, const std::string &subtitle, const std::string &value,
                   int y, int height) {
        const auto shown = value.empty() ? std::string("›") : value + " ›";
        const int value_width = std::min(text_width(shown, 30), 300);
        const int title_width = 612 - value_width - 16;
        if (subtitle.empty())
            text(title, 36, y + (height - 30) / 2 - 6, 30, title_width);
        else {
            text(title, 36, y + height / 2 - 46, 30, title_width);
            text(subtitle, 36, y + height / 2 + 6, 24, title_width, 3);
        }
        text(shown, 648 - value_width, y + (height - 30) / 2 - 6, 30, value_width, 3);
        canvas.line(36, y + height - 1, 647, y + height - 1, 12, 2);
        hits.push_back({24, y, 636, height, id});
    }
    void draw_import() {
        if (!bookmark_io_ready()) return;
        view = View::Import;
        hits.clear();
        canvas.clear();
        header("网络与传输", Back, import_from_settings ? "设置" : "书库");
        text("USB 文件传输", 36, 140, 24, 612, 3);
        const int usb = wrapped("电脑可直接管理 TF 卡。传输期间暂停阅读和同步，退出时会重启。", 36, 180, 612, 30);
        button(UsbStart, "开启 USB 传输", 24, 204 + usb, 636, 96, false, 30);
        int y = 340 + usb;
        canvas.line(36, y, 647, y, 12, 2);
        text("Wi-Fi", 36, y + 36, 24, 612, 3);
        y += 76;
        std::vector<provision::SavedWifi> networks;
        std::string detail;
        const bool readable = provision::load_networks(root, networks, detail);
        if (!wifi_enabled()) {
            wrapped("Wi-Fi 已关闭。打开后可以传书和同步。", 36, y + 20, 612, 28);
            button(WifiSettings, "Wi-Fi 设置", 24, y + 156, 636, 96);
        } else if (readable && !networks.empty()) {
            entry_row(WifiSettings, "Wi-Fi 设置", "已保存 " + std::to_string(networks.size()) + " 个网络", network_label(), y, 120);
            text("Wi-Fi 传书", 36, y + 156, 24, 612, 3);
            entry_row(WifiUploadStation, "经已保存的 Wi-Fi 传书", "手机连同一网络，传书时仍能上网", "", y + 196, 120);
            entry_row(WifiUpload, "经设备热点传书", "不需要路由器，手机暂时不能上网", "", y + 316, 120);
        } else {
            const int height = readable ? 104 : 120;
            entry_row(WifiSettings, "Wi-Fi 设置", readable ? "" : "PicoPage/network.toml 无效",
                      readable ? "未添加网络" : "", y, height);
            entry_row(WifiUpload, "Wi-Fi 传书", "经设备热点，不需要路由器", "", y + height, 120);
            wrapped("添加 Wi-Fi 后，也可以经已保存的网络传书，手机传书时仍能上网。", 36, y + height + 140, 612, 24, true, 3);
        }
        // Start failures name their cause in several clauses, so the notice wraps.
        if (!notice().empty()) {
            canvas.line(0, 1012, 683, 1012, 1, 3);
            wrapped(message, 36, 1044, 612, 24);
        }
    }
    /** Saved-file summary of an upload session: the count and the latest file name. */
    void upload_summary(int y, int title_size) {
        const bool any = portal.saved_books > 0;
        text(portal_closing ? "正在结束…" : portal.phase == PortalPhase::Receiving ? "正在接收 / 保存…" : any ? "已保存 " + std::to_string(portal.saved_books) + " 个文件" : "等待手机上传",
             36, y, title_size, 612, 1, true);
        if (!portal_closing) {
            if (any) {
                text("最近：" + portal.latest_book, 36, y + title_size + 22, 24, 612, 3);
                text("逐个结果见浏览器。", 36, y + title_size + 56, 24, 612, 3);
            } else
                wrapped("逐个结果见浏览器。结束后书库和字体列表会更新。", 36, y + title_size + 22, 612, 24, true, 3);
        }
    }
    void draw_portal() {
        view = View::Portal;
        hits.clear();
        canvas.clear();
        header("Wi-Fi 传书", 0, "", 420);
        if (!portal_closing) wifi_link(44);
        const auto scheme = portal.url.find("://");
        const auto address = scheme == std::string::npos ? portal.url : portal.url.substr(scheme + 3);
        if (portal.url.empty()) {
            note(portal_closing ? "正在结束…" : portal_station ? "正在连接已保存的 Wi-Fi…" : "正在开启设备热点…", 200);
        } else if (!portal.network.empty()) {
            text("用手机扫码打开传书页", 36, 140, 30, 612, 1, true);
            wrapped("手机或电脑需连着“" + portal.network + "”。", 36, 188, 612, 24, true, 3);
            graphics::draw_qr(canvas, portal.url, 152, 256, 380);
            center("或在浏览器输入", 0, 660, 684, 24, 3);
            center(address, 0, 700, 684, 34);
            canvas.line(0, 780, 683, 780, 1, 3);
            upload_summary(808, 30);
        } else {
            text("1  用手机连接设备热点", 36, 128, 26, 612, 1, true);
            graphics::draw_qr(canvas, provision::wifi_qr({portal.access_ssid, portal.access_password}), 36, 176, 264);
            text("热点名称", 324, 196, 22, 324, 3);
            text(portal.access_ssid, 324, 230, 28, 324);
            text("密码", 324, 300, 22, 324, 3);
            text(portal.access_password, 324, 334, 28, 324);
            text("扫码可直接连接", 324, 400, 22, 324, 3);
            text("2  连接后扫码打开传书页", 36, 472, 26, 612, 1, true);
            graphics::draw_qr(canvas, portal.url, 36, 520, 264);
            text("或在浏览器输入", 324, 540, 22, 324, 3);
            text(address, 324, 574, 28, 324);
            text("书籍、字体、锁屏图片", 324, 660, 22, 324, 3);
            canvas.line(0, 816, 683, 816, 1, 3);
            upload_summary(844, 28);
        }
        if (!portal_closing) button(PortalStop, "结束传书", 24, 1080, 636, 96);
    }
    void draw_usb() {
        view = View::UsbTransfer;
        hits.clear();
        canvas.clear();
        // TF ownership may already belong to the host; this page uses no card resources.
        if (usb_confirm) {
            confirm_page("USB 文件传输中", "退出 USB 传输？", "", "",
                         "请先在电脑上安全弹出设备。确认后设备会重启，并回到书库。");
            button(UsbCancel, "取消", 24, 1080, 306, 96);
            button(UsbExitConfirm, "退出并重启", 354, 1080, 306, 96, true);
        } else {
            header(usb_active ? "USB 文件传输中" : "USB 文件传输", 0);
            text(usb_active ? "电脑正在使用 TF 卡" : "USB 文件传输未开启", 36, 160, 40, 612, 1, true);
            const int height = wrapped("传输期间设备暂停阅读和同步。完成后先在电脑上安全弹出，再退出并重启。", 36, 244, 612, 28);
            if (!message.empty()) wrapped(message, 36, 272 + height, 612, 26);
            button(UsbExit, "退出并重启", 24, 1080, 636, 96);
        }
    }
    static bool same_account(const sync::Config &a, const sync::Config &b) {
        return a.endpoint == b.endpoint && a.username == b.username && a.userkey == b.userkey &&
               a.device_id == b.device_id && a.dom_version == b.dom_version;
    }
    void cancel_sync() {
        network::release(sync_hold);
        cancel_account();
        if (active_network && uploading && upload_record) {
            uncertain_record = upload_record;
            uncertain_config = sync_config;
        }
        if (network_cancel)
            network_cancel->store(true);
        active_network = 0;
        network_request.reset();
        remote_anchor.reset();
        remote_place.reset();
        upload_record.reset();
        sync_checked = false;
        uploading = false;
        sync_state = SyncState::Unchecked;
        remote_percent = -1;
        remote_device.clear();
    }
    std::string anchor_label(const Anchor& anchor) const {
        for (const auto& item : document.toc())
            if (item.resource == anchor.resource) return item.label;
        return basename(anchor.resource);
    }
    static std::string percent_text(double fraction) {
        return std::to_string(static_cast<int>(std::lround(std::clamp(fraction, 0., 1.) * 100))) + "%";
    }
    std::string local_position() const { return chapter_label() + " · " + book_percent(); }
    /**
     * Spine index of the mapped remote position; false until a mapping supplied its place. Also
     * false when the remote or the loaded item is non-linear: such an item is outside the
     * reading order, so it has neither a book percentage nor an order against another position.
     */
    bool remote_chapter(std::size_t& index) const {
        const auto& spine = document.spine();
        return remote_anchor && remote_place && document.chapter_index_for(remote_anchor->resource, index) &&
               spine[index].linear && chapter_index < spine.size() && spine[chapter_index].linear;
    }
    /** Percentage of the mapped remote position, computed as book_percent() is; empty when unmapped. */
    std::string remote_book_percent() const {
        std::size_t index;
        if (!remote_chapter(index)) return {};
        return std::to_string(book_percent_value(index, *remote_place, remote_place->offset)) + "%";
    }
    /** The remote position: its chapter once mapped, otherwise only the percentage the server reported. */
    std::string remote_position() const {
        if (remote_anchor) {
            const auto percent = remote_book_percent();
            return anchor_label(*remote_anchor) + (percent.empty() ? "" : " · " + percent);
        }
        return remote_percent >= 0 ? "约 " + percent_text(remote_percent) : "位置未知";
    }
    /**
     * 1 when the mapped remote position lies after the visible page, -1 when it lies before the
     * page, 0 when it is on the page or was not mapped. Spine item, chapter part and offset are
     * compared; percentages are not.
     */
    int further() const {
        std::size_t index;
        if (!remote_chapter(index)) return 0;
        if (index != chapter_index) return index > chapter_index ? 1 : -1;
        if (remote_place->part != chapter.part) return remote_place->part > chapter.part ? 1 : -1;
        return remote_place->offset >= page.end ? 1 : remote_place->offset < page.start ? -1 : 0;
    }
    /** Returns the sync panel to its unchecked state after a cancelled request. */
    void sync_cancelled(bool sent) {
        cancel_sync();
        sync_status = sent ? "已取消。已发出的上传仍可能生效，请重新检查远端。" : "已取消。本机位置没有改动。";
    }
    void draw_network_closing() {
        const bool account_page_waiting = network_return_view == View::SyncSettings;
        view = View::NetworkClosing;
        hits.clear(); canvas.clear(); header(account_page_waiting ? "KOReader 同步" : "同步", 0);
        note(account_page_waiting ? "正在结束验证…" : "正在结束同步…", 246);
        wrapped(account_page_waiting ? "网络任务正在安全收尾。账户没有保存。"
                                     : "网络任务正在安全收尾。已发出的上传仍可能生效，下次同步需重新检查远端。", 36, 336, 612, 26);
        button(ContinueOffline, "继续离线阅读", 24, 1080, 636, 96);
    }
    void wait_network(int next_control) {
        network_after_cancel = next_control;
        network_return_view = view;
        sync_cancelled(uploading);
        draw_network_closing(); overlay();
    }
    // Sync panel slots below the two columns: status text, the network row and two button rows.
    // The upper row holds checks and other actions that change nothing; uploads sit in the lower
    // row, so a second tap on "check" never lands on an upload.
    static constexpr int sync_status_top = 816, sync_network_top = 884, sync_upper = 952, sync_lower = 1052;
    void sync_columns(const std::string& remote_value, const std::string& remote_detail) {
        canvas.line(342, 610, 342, 795, 1, 2);
        canvas.line(0, 795, 683, 795, 1, 2);
        text("本机", 36, 634, 24, 286, 3);
        title_lines(local_position(), 36, 678, 286, 28, true, 1);
        text(session.needs_save() ? "尚未保存" : "这台设备", 36, 740, 22, 286, 3);
        text("远端", 362, 634, 24, 286, 3);
        title_lines(remote_value, 362, 678, 286, 28, true, 1);
        text(remote_detail, 362, 740, 22, 286, 3);
    }
    void sync_network_row() {
        value_row(WifiModeOpen, "网络", "", network_label(), sync_network_top, 60);
    }
    void draw_sync() {
        if (!bookmark_io_ready()) return;
        view = View::Sync;
        if (!has_book()) {
            hits.clear(); canvas.clear(); header("同步", Back, "书库");
            note("请先打开一本 EPUB。", 246); return;
        }
        // content_loading here means that a position is being mapped for this panel.
        const bool busy = active_network || content_loading;
        if (sync_state == SyncState::Different && remote_anchor && !busy) {
            // Each card is the control for its position; nothing changes on the server here.
            if (!panel_background(508, false)) return;
            canvas.line(0, 508, 683, 508, 1, 3);
            text("两端位置不同", 36, 550, 32, 612, 1, true);
            note("点选要继续的位置。不会自动改动远端。", 612);
            const int order = further();
            outline(36, 674, 297, 250, order < 0 ? 3 : 2); outline(351, 674, 297, 250, order < 0 ? 2 : 3);
            text("本机", 56, 694, 24, 258, 3);
            title_lines(chapter_label(), 56, 738, 256, 28, true, 2);
            text(book_percent(), 56, 806, 24, 256);
            text(order < 0 ? "这台设备 · 更靠后" : "这台设备", 56, 838, 22, 256, 3);
            canvas.line(36, 872, 332, 872, 12, 2);
            center("留在这里", 36, 882, 297, 24);
            hits.push_back({36, 674, 297, 250, SyncStay});
            text("远端", 371, 694, 24, 256, 3);
            title_lines(anchor_label(*remote_anchor), 371, 738, 256, 28, true, 2);
            text(remote_book_percent(), 371, 806, 24, 256);
            const auto device = remote_device.empty() ? std::string("其他设备") : remote_device;
            text(order > 0 ? device + " · 更靠后" : device, 371, 838, 22, 256, 3);
            canvas.line(351, 872, 647, 872, 12, 2);
            center("跳到这里", 351, 882, 297, 24);
            hits.push_back({351, 674, 297, 250, SyncJump});
            // A refused or failed upload returns to this panel, which then shows its reason.
            if (sync_status.empty()) note("留在这里不会上传，下次检查仍会提示。", 948);
            else title_lines(sync_status, 36, 948, 612, 24, false, 2);
            button(SyncUpload, "上传本机位置…", 36, sync_lower, 612, 88);
            return;
        }
        if (!panel_background(508 - panel_strip)) return;
        tabs(508, Sync);
        if (txt_name(book_path)) {
            const int height = wrapped("TXT 的阅读位置只保存在本机，不支持 KOReader 接续。", 36, 650, 612, 28);
            text(local_position() + (session.needs_save() ? " · 尚未保存" : " · 已保存"), 36, 674 + height, 24, 612, 3);
            return;
        }
        if (busy) {
            text(uploading ? "正在上传本机位置" : content_loading ? "正在准备位置" : "正在检查远端位置", 36, 650, 30, 612, 1, true);
            text("网络：" + network_label(), 36, 712, 24, 612, 3);
            // The frame stays unchanged until the request ends: panel updates during TLS traffic
            // have caused display line-queue underruns.
            button(SyncCancel, uploading ? "取消上传" : "取消同步", 36, sync_lower, 612, 88);
            if (content_loading) loading_cancel = hits.back();
            note(uploading ? "已经发出的上传，取消后服务器仍可能收到。" : "取消后保留本机位置。", 1160);
            return;
        }
        const auto status = [&](const std::string& fallback) {
            title_lines(sync_status.empty() ? fallback : sync_status, 36, sync_status_top, 612, 24, false, 2);
        };
        if (!badge_configured) {
            sync_columns("—", "未设置同步");
            status("还没有设置 KOReader 同步。设置后，可以和其他设备接续阅读位置。");
            button(SyncAccount, "设置同步账户", 36, sync_upper, 612, 88, true);
            return;
        }
        if (!wifi_enabled()) {
            sync_columns("未检查", "—");
            status("Wi-Fi 已关闭。");
            button(SyncEnable, "打开 Wi-Fi 并检查", 36, sync_upper, 612, 88, true);
            return;
        }
        switch (sync_state) {
        case SyncState::NoRecord:
            sync_columns("无记录", "服务器上没有这本书");
            status("上传后，其他设备打开同一本书时可以接续。");
            button(SyncUpload, "上传本机位置…", 36, sync_lower, 612, 88, true);
            break;
        case SyncState::Same:
            sync_columns(local_position(), "与本机相同");
            status("两端位置一致，不需要操作。");
            button(SyncCheck, "再次检查", 36, sync_upper, 612, 88);
            break;
        case SyncState::Unmapped:
            sync_columns(remote_position(), remote_device);
            status("这台设备上找不到远端位置对应的段落，没有跳转。两端的书可能不是同一版本。");
            button(Back, "继续阅读", 36, sync_upper, 612, 88, true);
            button(SyncUpload, "上传本机位置…", 36, sync_lower, 612, 88);
            break;
        case SyncState::Uploaded:
            sync_columns(local_position(), "刚刚上传");
            status("已上传。其他设备下次同步会收到这个位置。");
            button(SyncCheck, "再次检查", 36, sync_upper, 612, 88);
            break;
        case SyncState::Uncertain:
            sync_columns("未确认", "—");
            status("不确定服务器是否收到了这次上传。重新检查后再决定是否上传。");
            sync_network_row();
            button(SyncCheck, "重新检查", 36, sync_upper, 612, 88, true);
            break;
        case SyncState::Unauthorized:
            sync_columns("未检查", "—");
            status("账户或密码不正确，服务器拒绝了请求。");
            sync_network_row();
            button(SyncAccount, "同步账户设置", 36, sync_upper, 612, 88, true);
            break;
        case SyncState::Failed:
            sync_columns("未检查", "—");
            status("服务器没有响应，或网络中断。");
            sync_network_row();
            button(SyncCheck, "重试", 36, sync_upper, 612, 88, true);
            break;
        case SyncState::LinkFailed:
            sync_columns("未检查", "—");
            status("没有连上 Wi-Fi。");
            sync_network_row();
            button(SyncCheck, "重试", 36, sync_upper, 612, 88, true);
            // A pinned network is never replaced silently; the user may allow it for this check.
            if (sync_link_pinned) button(SyncOnceAuto, "本次改用自动", 36, sync_lower, 612, 88);
            else button(WifiSettings, "Wi-Fi 设置", 36, sync_lower, 612, 88);
            break;
        case SyncState::ConfigChanged:
            sync_columns("未检查", "—");
            status("同步账户已更改，请重新检查。");
            button(SyncCheck, "检查远端位置", 36, sync_upper, 612, 88, true);
            break;
        default:
            sync_columns("未检查", "");
            status(sync_badge() == "待核实" ? "上次上传结果未确认，请重新检查远端。" : "先读取远端位置，再决定是否上传。");
            sync_network_row();
            button(SyncCheck, "检查远端位置", 36, sync_upper, 612, 88, true);
        }
    }
    /** Starts one check. automatic lets this check alone use any saved network instead of the pinned one. */
    void check_sync(bool automatic = false) {
        if (txt_name(book_path)) { draw_sync(); return; }
        cancel_sync();
        sync_status.clear();
        std::string detail;
        refresh_badge_config();
        sync::HttpRequest request;
        if (!wifi_enabled() || !sync::load_config(root, sync_config, detail)) {
            // The panel shows the closed radio or the missing account with its own action.
        } else if (!sync::get_request(sync_config, document.metadata().partial_md5, request, detail)) {
            sync_state = SyncState::Failed;
            sync_status = "无法生成同步请求，请检查同步账户。";
        } else {
            auto targets = wifi_targets(detail, automatic);
            if (targets.networks.empty()) {
                sync_state = SyncState::LinkFailed; sync_link_pinned = false;
                sync_status = detail;
            } else {
                sync_link_pinned = !targets.automatic;
                sync_link_name = targets.automatic ? "" : targets.networks.front().ssid;
                sync_hold = std::make_shared<std::atomic_bool>(false);
                active_network = ++network_sequence;
                uploading = false;
                network_cancel = std::make_shared<std::atomic_bool>(false);
                network_request = NetworkOperation{active_network, std::move(request), network_cancel, sync_hold, std::move(targets)};
            }
        }
        draw_sync();
        overlay();
    }
    void confirm_upload() {
        const bool replaces = sync_state == SyncState::Different || sync_state == SyncState::Unmapped;
        const auto local = local_position();
        const auto remote = remote_position();
        const auto device = remote_device.empty() ? std::string("其他设备") : remote_device + " ";
        if (!panel_background(752, false)) return;
        view = View::Confirmation;
        canvas.line(0, 752, 683, 752, 1, 3);
        if (replaces) {
            text("用本机位置替换远端？", 36, 786, 32, 612, 1, true);
            text("本机　" + local, 36, 852, 26, 612);
            text("远端　" + remote + (remote_device.empty() ? "" : "（" + remote_device + "）"), 36, 896, 26, 612);
            wrapped("替换后，" + device + "下次同步会回到 " + book_percent() + "。", 36, 952, 612, 24, true, 3);
            // Cancel is the filled default; replacing stays an ordinary, available button.
            button(Back, "取消", 24, 1080, 306, 96, true);
            button(SyncConfirm, "替换", 354, 1080, 306, 96);
        } else {
            text("上传本机位置？", 36, 786, 32, 612, 1, true);
            wrapped(local + " 将保存到服务器。其他设备下次同步会收到这个位置。", 36, 852, 612, 26);
            button(Back, "取消", 24, 1080, 306, 96);
            button(SyncConfirm, "上传", 354, 1080, 306, 96, true);
        }
    }
    void begin_mapping(ContentJob::Kind kind, const std::string& xpointer = "") {
        abandon_content(); invalidate_counting(); release_counting(); clear_prefetch();
        ContentJob::Request request;
        request.kind = kind; request.path = root + "/" + book_path; request.identity = document.metadata();
        request.dom_version = sync_config.dom_version; request.xpointer = xpointer;
        if (kind == ContentJob::Kind::MapUpload) request.record = session.saved_record();
        request.generation = foreground_generation;
        pending_content = std::move(request); content_loading = true;
        // The panel itself shows the work and its cancel control.
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
        ESP_LOGI("reader_sync", "mapping kind=%s", kind == ContentJob::Kind::MapDownload ? "download" : "upload");
#endif
        draw_sync();
        if (view != View::Sync) { abandon_content(); return; }
        view = View::Loading;
        overlay();
    }
    void accept_remote_mapping(const Anchor& candidate, const sync::PartPosition& place, const std::string& detail) {
        sync_status.clear();
        remote_place.reset();
        if (detail.empty()) {
            remote_anchor = candidate;
            if (session.saved_record() && session.saved_record()->anchor == candidate) {
                network::release(sync_hold);
                remote_anchor.reset(); sync_state = SyncState::Same;
            } else { remote_place = place; sync_state = SyncState::Different; }
        } else {
            remote_anchor.reset(); sync_state = SyncState::Unmapped;
            log_mapping_failure("Download", detail);
            if (memory_error(detail)) sync_status = "设备内存不足，暂时无法定位远端位置，没有跳转。";
            else if (outside_mapping(detail))
                sync_status = "远端位置所在的章节含有表格等暂不支持的内容，无法换算成本机位置，没有跳转。";
        }
    }
    /** True when the content at a position, not a transient condition, keeps it from being mapped. */
    static bool outside_mapping(const std::string& detail) {
        return detail == sync::kUnsupportedStructure || detail == sync::kUnresolvedVisibility;
    }
    /**
     * Records why a position could not be mapped. The texts are fixed and name at most a
     * resource inside the EPUB; they hold no reading text, position or account data.
     */
    static void log_mapping_failure([[maybe_unused]] const char* direction, [[maybe_unused]] const std::string& detail) {
#ifdef ESP_PLATFORM
        ESP_LOGW("reader_sync", "%s mapping failed: %s", direction, detail.c_str());
#endif
    }
    /** Reports a saved position that could not be turned into a KOReader position; nothing was sent. */
    void upload_mapping_failed(const std::string& detail) {
        log_mapping_failure("Upload", detail);
        sync_status = memory_error(detail) ? "设备内存不足，暂时无法计算上传位置。" :
            outside_mapping(detail) ? "这一章含有表格等暂不支持的内容，无法换算成 KOReader 的位置，没有上传。" :
            "这个位置暂时不能上传。";
    }
    void upload() {
        if (!sync_checked || !persist() || !session.saved_record()) {
            sync_status = "当前进度尚未保存，无法上传。";
            draw_sync();
            overlay();
            return;
        }
        std::string detail;
        sync::Config current;
        if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
            cancel_sync();
            sync_state = SyncState::ConfigChanged;
            draw_sync();
            overlay();
            return;
        }
        const auto record = *session.saved_record();
        if (async_content) { begin_mapping(ContentJob::Kind::MapUpload); return; }
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
            upload_mapping_failed(detail);
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
        send_upload(record, xpointer, percent);
    }
    void send_upload(const ReadingRecord& record, const std::string& xpointer, double percent) {
        // The upload belongs to the session that answered the check; an ended session checks again.
        if (!network::live(sync_hold)) { check_sync(); return; }
        if (!wifi_enabled()) { cancel_sync(); draw_sync(); overlay(); return; }
        std::string detail;
        sync::HttpRequest request;
        if (!sync::put_request(sync_config, document.metadata().partial_md5, xpointer, percent,
                               record, request, detail)) {
            sync_status = "无法生成同步请求，请检查同步账户。";
            draw_sync();
            overlay();
            return;
        }
        // The link of this session is already up, so the worker uses it without choosing again.
        auto targets = wifi_targets(detail, !sync_link_pinned);
        upload_record = record;
        uploading = true;
        active_network = ++network_sequence;
        network_cancel = std::make_shared<std::atomic_bool>(false);
        network_request = NetworkOperation{active_network, std::move(request), network_cancel, sync_hold, std::move(targets)};
        sync_status.clear();
        draw_sync();
        overlay();
    }
    void network_complete(std::uint64_t id, int status, const std::string &bytes,
                          const std::string &failure, LinkFailure link) {
        if (id != active_network || !active_network)
            return;
        if (account_check) { account_complete(status, failure, link); return; }
        if (uploading && upload_record) { uncertain_record = upload_record; uncertain_config = sync_config; }
        active_network = 0;
        if (view != View::Sync) {
            cancel_sync();
            return;
        }
        sync::Config current;
        std::string detail;
        sync_status.clear();
        if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
            cancel_sync();
            sync_state = SyncState::ConfigChanged;
        } else if (!failure.empty()) {
            sync_checked = false;
            remote_anchor.reset();
            // A failed upload may have reached the server, whatever the reported cause.
            sync_state = uploading ? SyncState::Uncertain : link != LinkFailure::None ? SyncState::LinkFailed : SyncState::Failed;
            if (sync_state == SyncState::LinkFailed)
                sync_status = sync_link_pinned && link == LinkFailure::NotNearby
                    ? "指定的" + quoted_name(sync_link_name) + "不在附近，没有连接其他网络。" : failure;
        } else if (uploading) {
            remote_anchor.reset();
            if (sync::upload_confirmed(status, detail) && upload_record && session.saved_record() &&
                *session.saved_record() == *upload_record) {
                acknowledged_record = upload_record; acknowledged_config = sync_config; uncertain_record.reset();
                sync_state = SyncState::Uploaded;
            } else {
                uncertain_record = upload_record; uncertain_config = sync_config;
                sync_state = status == 401 || status == 403 ? SyncState::Unauthorized : SyncState::Uncertain;
            }
            sync_checked = false;
        } else {
            const auto remote = sync::parse_remote(status, bytes);
#ifdef CONFIG_PICO_PAGE_LEGACY_PERF_LOG
            const char* remote_status = remote.status == sync::RemoteStatus::Found ? "Found" :
                remote.status == sync::RemoteStatus::NoRecord ? "NoRecord" :
                remote.status == sync::RemoteStatus::Unauthorized ? "Unauthorized" : "Error";
            ESP_LOGI("reader_sync", "http_status=%d remote_status=%s", status, remote_status);
#endif
            if (remote.status == sync::RemoteStatus::NoRecord) {
                uncertain_record.reset(); acknowledged_record.reset();
                sync_checked = true; sync_state = SyncState::NoRecord;
            } else if (remote.status == sync::RemoteStatus::Found) {
                uncertain_record.reset(); acknowledged_record.reset();
                Anchor candidate;
                sync::PartPosition place;
                sync_checked = true;
                remote_percent = remote.percentage;  // parse_remote() accepts only [0, 1].
                remote_device = remote.device;
                if (async_content) { begin_mapping(ContentJob::Kind::MapDownload, remote.progress); return; }
                sync::from_xpointer(document, remote.progress, sync_config.dom_version, candidate, detail, {}, &place);
                accept_remote_mapping(candidate, place, detail);
            } else {
                sync_checked = false;
                sync_state = remote.status == sync::RemoteStatus::Unauthorized ? SyncState::Unauthorized : SyncState::Failed;
            }
        }
        if (!sync_checked || sync_state == SyncState::Same) network::release(sync_hold);
        uploading = false;
        upload_record.reset();
        draw_sync();
        overlay();
    }
    /**
     * Chinese wording for a failure detail produced by the document, store or sync layers. Their
     * details are English diagnostic text for the log; text that is already Chinese passes through.
     * A colon-separated detail may also include a non-ASCII resource path. The layers report no
     * error codes; an unknown diagnostic uses the caller's fallback or a generic failure message.
     */
    static std::string reason(const std::string &detail, const char *fallback = "操作失败") {
        std::string lower = detail;
        for (auto &c : lower) if (c >= 'A' && c <= 'Z') c += 32;
        const auto has = [&](const char *word) { return lower.find(word) != std::string::npos; };
        if (lower == "missing epub mimetype") return "EPUB 缺少格式标识";
        if (lower == "epub mimetype exceeds limit") return "EPUB 格式标识过长";
        if (lower == "cannot read epub mimetype") return "EPUB 格式标识读取失败";
        if (lower == "invalid epub mimetype") return "EPUB 格式标识不正确";
        if (has("memory") || has("alloc")) return "设备内存不足";
        if (has("changed")) return "文件在读取过程中发生了变化";
        if (has("unsupported") || has("requires") || has("exactly") || has("not retained")) return "内容结构暂不支持";
        if (has("unavailable") || has("no longer") || has("cannot be read") || has("read failed") ||
            has("cannot open") || has("cannot seek") || has("cannot hash") || has("cannot read"))
            return "文件无法读取，请检查 TF 卡";
        if (detail.find(": ") == std::string::npos &&
            std::any_of(detail.begin(), detail.end(), [](unsigned char c) { return c >= 0x80; })) {
            const std::string stop = "。";
            const bool ended = detail.size() >= stop.size() && detail.compare(detail.size() - stop.size(), stop.size(), stop) == 0;
            return ended ? detail.substr(0, detail.size() - stop.size()) : detail;
        }
        return fallback;
    }
    std::string retry_path;
    /**
     * Shows a failure. The text up to the first full stop, comma or colon is the specific title;
     * the rest explains it. retry names a book whose opening failed and can be repeated.
     */
    void error(const std::string &text_in, const std::string &retry = "") {
        image_viewing = false;
        viewer_original.reset();
        image_contrast_level = 0;
        // The argument may alias message, which is assigned below.
        const std::string detail = text_in;
#ifdef ESP_PLATFORM
        ESP_LOGW("reader_app", "Error shown: %s", detail.c_str());
#endif
        abandon_content();
        if (drawing_error) return;
        drawing_error = true;
        // The page offers a save retry only for a failed commit, not for one still pending.
        if (card_available && session.needs_save() && !save_failed)
            persist();
        cancel_sync();
        link_return = confirmed_link_return;
        message = detail;
        retry_path = retry;
        std::size_t split = std::string::npos, mark = 0;
        for (const char *separator : {"。", "，", "："}) {
            const auto found = detail.find(separator);
            if (found < split) { split = found; mark = std::strlen(separator); }
        }
        const auto title = detail.substr(0, split);
        const auto body = split == std::string::npos ? std::string() : detail.substr(split + mark);
        std::uint32_t visible_offset;
        const bool available = session.visible_record() && chapter.offset_for(session.visible_record()->anchor, visible_offset);
        const bool retained = (!async_content || available) && !foreground_job && card_available && has_book() && session.visible_record() &&
                              restore_confirmed_position() && panel_background(752, false);
        view = View::Error;
        hits.clear();
        const bool unsaved = session.needs_save();
        const bool resumes = has_book() && (session.visible_record() || session.saved_record());
        if (retained) {
            canvas.fill_rect(0, 752, 684, 464, 15);
            canvas.line(0, 752, 683, 752, 1, 3);
            text(title, 36, 784, 32, 612, 1, true);
            title_lines(body, 36, 850, 612, 26, false, 3);
            note("保留最后确认的位置。", 1012);
        } else {
            canvas.clear();
            header(retry.empty() ? "暂时无法继续" : "无法打开", unsaved || resumes ? 0 : Back, "书库");
            title_lines(title, 36, 160, 612, 40, true);
            int y = 160 + std::min(wrapped(title, 0, 0, 612, 40, false), 96) + 28;
            if (!body.empty()) y += wrapped(body, 36, y, 612, 28) + 28;
            if (!retry.empty()) {
                y += wrapped(retry, 36, y, 612, 24, true, 3);
                wrapped("可重试一次；仍失败时请检查 TF 卡。", 36, y, 612, 24, true, 3);
            }
        }
        // Two actions share the row with the recommended one on the right; one action fills it.
        const auto leave = unsaved || !card_available ? "返回" : resumes ? "恢复阅读" : "返回书库";
        const bool second = unsaved || !retry.empty();
        button(Back, leave, 24, 1080, second ? 306 : 636, 96, !second);
        if (unsaved) button(SaveRetry, "重试保存", 354, 1080, 306, 96, true);
        else if (!retry.empty()) button(Retry, "重试", 354, 1080, 306, 96, true);
        drawing_error = false;
        if (!display.busy()) overlay();
    }
    void back() {
        if (view == View::NetworkClosing) return;
        if (network_worker_id && active_network) { wait_network(Back); return; }
        if (view == View::TextEntry) {
            const auto field = entry.field;
            entry = {};
            if (field == TextEntry::Field::WifiPassword && join_manual) {
                // The name stays so that the first step can be corrected.
                join_draft.password.clear();
                open_entry(TextEntry::Field::WifiName, join_draft.ssid);
                return;
            }
            if (field == TextEntry::Field::WifiPassword || field == TextEntry::Field::WifiName) {
                leave_join(join_origin);
                draw_wifi();
            } else draw_sync_settings();
            overlay();
            return;
        }
        if (view == View::Update) {
            if (update_page == UpdatePage::Notes) { update_page = notes_return; draw_update(); overlay(); return; }
            // The other pages end through their own buttons.
            if (update_page != UpdatePage::Home && update_page != UpdatePage::Failed) return;
            update_page = UpdatePage::Home; update_notice.clear();
            draw_settings(); overlay(); return;
        }
        if (view == View::WifiSettings) {
            if (portal_live) { draw_portal(); overlay(); return; }
            if (wifi_page == WifiPage::Joining || wifi_page == WifiPage::JoinFailed || wifi_page == WifiPage::Joined)
                return;  // These pages end through their own buttons.
            if (wifi_page != wifi_root) {
                if (wifi_page == WifiPage::Add) {
                    // A scan in progress is abandoned; its late result carries a stale ID.
                    network::release(wifi_hold);
                    if (wifi_worker && wifi_kind == WifiOperation::Kind::Scan) { wifi_worker = 0; wifi_request.reset(); }
                }
                wifi_page = wifi_page == WifiPage::Detail ? WifiPage::Saved
                          : wifi_page == WifiPage::Forget ? WifiPage::Detail : WifiPage::Home;
                draw_wifi(); overlay(); return;
            }
            wifi_page = wifi_root = WifiPage::Home;
            if (std::exchange(wifi_from_sync, false)) { draw_sync(); overlay(); return; }
            if (std::exchange(wifi_from_transfer, false)) { draw_import(); overlay(); return; }
        }
        if (view == View::SyncSettings) {
            if (account_page == AccountPage::Remove) { account_page = AccountPage::Form; draw_sync_settings(); overlay(); return; }
            if (account_page == AccountPage::Verifying) { cancel_account(); draw_sync_settings(); overlay(); return; }
            std::fill(account.password.begin(), account.password.end(), '\0');
            account.password.clear();
            if (std::exchange(account_from_sync, false) && has_book()) {
                refresh_badge_config(); cancel_sync(); sync_status.clear(); draw_sync(); overlay(); return;
            }
        }
        if (view == View::LockScreens) { draw_power(); overlay(); return; }
        if (view == View::PowerMenu) { close_power_menu(); return; }
        if (view == View::Portal) return;
        if (view == View::BookManagement) { draw_details(); overlay(); return; }
        if (usb_prepared) {
            if (usb_confirm) { usb_confirm = false; draw_usb(); overlay(); }
            return;
        }
        if (view == View::RefreshConfirm || view == View::RefreshInterval || view == View::PageEffects) {
            draw_refresh(); overlay(); return;
        }
        if (view == View::Help && !help_from_settings) { draw_library(); overlay(); return; }
        if (view == View::Help || view == View::About || view == View::WifiSettings || view == View::PowerSettings || view == View::StorageSettings || view == View::InputSettings ||
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
                const auto parent = p == std::string::npos ? "" : folder.substr(0, p);
                folder = folder_root || parent == library_home_root ? "" : parent;
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
        if (!bookmark_io_ready()) return false;
        auto snapshot = store.settings_snapshot(global ? "" : book_path);
        if (!snapshot.config.diagnostics.empty()) {
            error("设置文件无效，原文件未修改。请检查后重新打开设置。");
            return false;
        }
        edit_snapshot = std::move(snapshot);
        return true;
    }
    /** Controls of the Wi-Fi pages, the keyboard page, the account page and the sync panel's network actions. */
    void wifi_control(int id) {
        const auto index = [&](int base) { return static_cast<std::size_t>(id - base); };
        std::string detail;
        if (id >= WifiNearbyBase) {
            if (view != View::WifiSettings || wifi_page != WifiPage::Add || index(WifiNearbyBase) >= wifi_nearby.size()) return;
            const auto& found = wifi_nearby[index(WifiNearbyBase)];
            join_draft = {found.ssid, ""};
            join_manual = false; join_open = found.open; join_secured = !found.open; join_origin = WifiPage::Add;
            if (join_open) join_wifi();
            else open_entry(TextEntry::Field::WifiPassword, "");
            return;
        }
        if (id >= WifiModeBase) {
            if (view != View::WifiSettings || wifi_page != WifiPage::Mode || index(WifiModeBase) >= wifi_saved.size()) return;
            wifi_notice = provision::save_selected_network(root, wifi_saved[index(WifiModeBase)].ssid, detail)
                ? "" : "连接方式未保存，请检查 TF 卡。";
            draw_wifi(); overlay(); return;
        }
        if (id >= WifiSavedBase) {
            if (view != View::WifiSettings || wifi_page != WifiPage::Saved || index(WifiSavedBase) >= wifi_saved.size()) return;
            wifi_choice = wifi_saved[index(WifiSavedBase)].ssid;
            wifi_page = WifiPage::Detail;
            draw_wifi(); overlay(); return;
        }
        switch (id) {
        case WifiConnect: connect_wifi(); return;
        case WifiDisconnect:
            network::release(manual_hold); cancel_sync();
            if (wifi_status.phase != WifiPhase::Offline) { wifi_status.phase = WifiPhase::Closing; wifi_notice = "正在结束…"; }
            else wifi_notice.clear();
            break;
        case WifiManage: wifi_page = WifiPage::Saved; wifi_notice.clear(); break;
        case WifiModeOpen: wifi_page = WifiPage::Mode; wifi_notice.clear(); break;
        case WifiModeAuto:
            // Without a pinned name the file already selects automatic connection.
            wifi_notice = wifi_target.empty() || provision::save_selected_network(root, "", detail)
                ? "" : "连接方式未保存，请检查 TF 卡。";
            break;
        case WifiAdd:
        case WifiRescan: scan_wifi(); return;
        case WifiManualName:
            network::release(wifi_hold);
            if (wifi_worker && wifi_kind == WifiOperation::Kind::Scan) { wifi_worker = 0; wifi_request.reset(); }
            join_draft = {};
            join_manual = true; join_open = join_secured = false; join_origin = WifiPage::Add;
            open_entry(TextEntry::Field::WifiName, "");
            return;
        case WifiUpdatePassword:
            if (!wifi_free()) break;
            join_draft = {wifi_choice, ""};
            join_manual = false; join_open = join_secured = false; join_origin = WifiPage::Detail;
            open_entry(TextEntry::Field::WifiPassword, "");
            return;
        case WifiForget: wifi_page = WifiPage::Forget; break;
        case WifiForgetConfirm:
            wifi_page = WifiPage::Detail;
            if (wifi_worker || network::live(manual_hold) || wifi_online()) wifi_notice = "请先断开 Wi-Fi，再忘记网络。";
            else if (!provision::save_network_edit(root, {wifi_choice, ""}, true, 0, detail)) wifi_notice = "忘记网络失败，请检查 TF 卡。";
            else {
                wifi_notice = wifi_choice == wifi_target ? "网络已忘记，连接方式已改回自动。" : "网络已忘记。";
                wifi_choice.clear();
                wifi_page = WifiPage::Saved;
            }
            break;
        case WifiJoinCancel:
            // The worker stops at its next cancellation check; its result carries a stale ID.
            network::release(wifi_hold);
            wifi_worker = 0; wifi_request.reset();
            // Every flow with a password page returns to it with its draft, also when the password is empty.
            if (!join_open) { open_entry(TextEntry::Field::WifiPassword, join_draft.password); return; }
            leave_join(join_origin);
            break;
        case WifiJoinRetry: join_wifi(); return;
        case WifiJoinEdit: open_entry(TextEntry::Field::WifiPassword, join_draft.password); return;
        case WifiJoinList: leave_join(join_origin == WifiPage::Detail ? WifiPage::Saved : WifiPage::Add); break;
        case WifiJoinDone: leave_join(WifiPage::Home); break;
        case SyncEnable: {
            if (!begin_settings_edit(true)) return;
            auto values = edit_snapshot.global_settings; values.wifi_enabled = true;
            if (store.apply_settings(edit_snapshot, values, false, detail) != StoreStatus::Ok) {
                sync_status = "Wi-Fi 开关未保存，请检查 TF 卡。"; draw_sync(); overlay(); return;
            }
            settings.wifi_enabled = true; check_sync(); return;
        }
        case SyncOnceAuto: check_sync(true); return;
        case SyncStay: {
            // The reading header keeps its reminder until the next check or another book.
            const auto reminder = remote_anchor;
            cancel_sync();
            remote_anchor = reminder;
            if (has_book()) recover_body();
            return;
        }
        case SyncAccount:
            cancel_sync();
            account_from_sync = true;
            open_account();
            draw_sync_settings(); overlay(); return;
        case AccountEndpoint: open_entry(TextEntry::Field::Endpoint, account.endpoint); return;
        case AccountUser: open_entry(TextEntry::Field::Username, account.username); return;
        case AccountPassword: open_entry(TextEntry::Field::Password, account.password); return;
        case AccountVerify: verify_account(); return;
        case AccountCancel: cancel_account(); draw_sync_settings(); overlay(); return;
        case AccountRemove: account_page = AccountPage::Remove; draw_sync_settings(); overlay(); return;
        case AccountRemoveConfirm: {
            if (!bookmark_io_ready()) return;
            const auto removed = store.remove_private("sync.toml");
            cancel_sync();
            open_account();
            refresh_badge_config();
            account_notice = removed == StoreStatus::Ok || removed == StoreStatus::Missing ? "同步账户已移除。" : "同步账户未移除，请检查 TF 卡。";
            draw_sync_settings(); overlay(); return;
        }
        case EntryClear: entry.value.clear(); entry.field_dirty = true; flush_entry(); return;
        case EntryMask: entry.masked = !entry.masked; entry.field_dirty = true; flush_entry(); return;
        case EntryDefault: entry.value = provision::default_endpoint; entry.field_dirty = true; flush_entry(); return;
        default: return;
        }
        draw_wifi(); overlay();
    }
    void control(int id) {
        if (view == View::Update && update_control(id)) return;
        if (id == UpdateOpen) { update_open(); return; }
        if (id == SleepCustom) { draft.sleep_screen = "custom"; draw_power(); overlay(); return; }
        if (id == SleepChoose || id == SleepRefresh) { list_lock_screens(); return; }
        if (id == SleepRotation) {
            draft.sleep_image.clear(); draft.sleep_screen = "custom"; draw_lock_screens(); overlay(); return;
        }
        if (view == View::LockScreens && id >= LockScreenBase && id < LockScreenBase + 256) {
            const auto index = static_cast<std::size_t>(id - LockScreenBase);
            if (index < lock_screens.size()) {
                draft.sleep_image = lock_screens[index]; draft.sleep_screen = "custom";
                draw_lock_screens(); overlay();
            }
            return;
        }
        if (view == View::NetworkClosing) {
            if (id == ContinueOffline) {
                network_after_cancel = 0;
                if (has_book()) recover_body();
                else { draw_library(); overlay(); }
            }
            return;
        }
        if (network_worker_id && active_network && id != FullRefresh) { wait_network(id); return; }
        if (view == View::Sync && (id == Contents || id == Typography || id == Refresh)) cancel_sync();
        if (usb_prepared && id != UsbExit && id != UsbExitConfirm && id != UsbCancel)
            return;
        if (id == WifiSettings || (id == WifiModeOpen && view == View::Sync)) {
            wifi_from_sync = view == View::Sync;
            // The network page reached from the library stays on the way back to the library.
            wifi_from_transfer = view == View::Import && !import_from_settings;
            if (!portal_live) cancel_sync();
            // The sync panel's network row opens the connection mode directly.
            wifi_page = wifi_root = id == WifiModeOpen ? WifiPage::Mode : WifiPage::Home;
            wifi_choice.clear();
            draw_wifi(); overlay(); return;
        }
        if (portal_live && (view == View::Portal || view == View::WifiSettings) && id != Back && id != FullRefresh) {
            if (id == PortalStop && !portal_closing) {
                portal_open_wifi = view == View::WifiSettings;
                portal_action = PortalAction::Stop;
                portal_closing = true;
                draw_portal(); overlay();
            }
            return;
        }
        if ((id == WifiUpload || id == UsbStart) && view == View::Library) import_from_settings = false;
        if (id == WifiUpload || id == WifiUploadStation) {
            if (!wifi_enabled()) {
                // The network page states that Wi-Fi is off and leads back to the library.
                if (view == View::Library) { draw_import(); overlay(); return; }
                wifi_page = wifi_root = WifiPage::Home; draw_wifi(); overlay(); return;
            }
            ++portal_generation;
            portal_requested = true;
            portal_request_view = view;
            portal_action = id == WifiUpload ? PortalAction::Upload : PortalAction::UploadStation;
            return;
        }
        if (id == UsbStart) {
            usb_request_view = view;
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
        if (id >= WifiConnect) { wifi_control(id); return; }
        if (id >= BookmarkBase) {
            const auto index = static_cast<std::size_t>(id - BookmarkBase);
            if (view != View::Contents || !bookmarks_usable() || index >= bookmarks.bookmarks.size()) return;
            const auto target = bookmarks.bookmarks[index].anchor;
            if (async_content) { clear_page_history(); begin_seek(target); return; }
            std::uint32_t position;
            if (!seek(target, position, "书签位置无法恢复，原书内容可能已更改")) return;
            clear_page_history();
            show_page(position);
            return;
        }
        if (id >= DetailBase) {
            const auto index = static_cast<std::size_t>(id - DetailBase);
            if (index < entries.size() && !entries[index].directory) {
                inspect_book(entries[index].path); if (!display.busy()) overlay();
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
        case Help:
            help_from_settings = view == View::Settings;
            message.clear(); help_page = 0; draw_help();
            break;
        case About:
            message.clear(); draw_about();
            break;
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
        case PowerMenu:
            if (!display.busy() && !active_network && !update_waiting() && !content_loading && !foreground_job && !usb_prepared && view != View::Portal) {
                if (counting_job && (counting_job->request.kind == ContentJob::Kind::SleepCover || counting_job->request.kind == ContentJob::Kind::LockScreen)) {
                    counting_job->cancel();
                    power_menu_wait = view;
                    return;
                }
                drain_save(); stop_covers(); clear_prefetch(); stop_counting_job();
                power_return_canvas = canvas; power_return_hits = hits; power_return_view = view;
                power_return_message = message; power_selected = PowerAction::None; power_save_failed = false;
                draw_power_menu(); overlay();
            }
            return;
        case PowerShutdown:
        case PowerRestart:
            if (view == View::PowerMenu && !power_save_failed) {
                power_selected = id == PowerRestart ? PowerAction::Restart : PowerAction::Shutdown;
                power_request = {power_selected, false};
            }
            return;
        case PowerForce:
            if (view == View::PowerMenu && power_save_failed)
                power_request = {power_selected, true};
            return;
        case PowerCancel:
            close_power_menu(); return;
        case BookmarkToggle:
            toggle_bookmark(); return;
        case ContentsToc:
        case ContentsBookmarks:
            if (view != View::Contents) return;
            contents_bookmarks = id == ContentsBookmarks;
            if (contents_bookmarks) {
                // Opens on the screen holding the first bookmark at or after the visible page.
                const auto present = page_bookmarks();
                bookmark_page = present.empty() ? 0 : present.front() / bookmarks_per_screen;
            } else toc_locate_current = true;
            draw_contents(); overlay(); return;
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
        case Retry:
            if (!retry_path.empty()) open(retry_path);
            return;
        case SyncCancel:
            sync_cancelled(uploading);
            draw_sync();
            break;
        case Library:
            if (!persist()) {
                error(message);
                return;
            }
            remember_position();
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
        case LibraryFolders: {
            if (!bookmark_io_ready()) return;
            const auto snapshot = store.settings_snapshot();
            auto values = snapshot.config.settings;
            values.library_folders = values.library_folders == "first" ? "recent" : "first";
            std::string detail;
            if (store.apply_settings(snapshot, values, false, detail) == StoreStatus::Ok) {
                library_page = 0;
                message = "文件夹排序已保存";
                scan_library();
            } else message = "文件夹排序未保存，请检查 TF 卡或设置文件";
            draw_input_settings(); break;
        }
        case LibrarySort: {
            if (!bookmark_io_ready()) return;
            const auto snapshot = store.settings_snapshot();
            auto values = snapshot.config.settings;
            values.library_sort = library_sort == "recent" ? "name" : library_sort == "name" ? "name_desc" : "recent";
            std::string detail;
            if (store.apply_settings(snapshot, values, false, detail) == StoreStatus::Ok) {
                library_sort = values.library_sort; settings.library_sort = library_sort;
                library_page = 0;
                message = library_sort == "recent" ? "按最近阅读排序" : library_sort == "name" ? "按书名升序排列" : "按书名降序排列";
                scan_library();
            } else message = "排序未保存，请检查 TF 卡或设置文件";
            draw_library(); break;
        }
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
        case PageEffects:
            draw_page_effects();
            break;
        case EffectOff: case EffectA: case EffectB: case EffectC: case EffectRipple:
            page_effect = id == EffectOff ? PageEffect::Off : id == EffectA ? PageEffect::TextTurnA :
                          id == EffectB ? PageEffect::TextTurnB : id == EffectC ? PageEffect::TextTurnC : PageEffect::Ripple;
            draw_page_effects();
            break;
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
            if (!bookmark_io_ready()) return;
            std::string detail;
            if (store.apply_settings(edit_snapshot, draft, false, detail) != StoreStatus::Ok) {
                error("休眠设置未保存。" + reason(detail) + "。"); return;
            }
            settings.auto_sleep_minutes = draft.auto_sleep_minutes;
            settings.sleep_screen = draft.sleep_screen;
            settings.sleep_image = draft.sleep_image;
            custom_prepare_finished = custom_sleep_attempted = false;
            custom_notice.clear(); custom_notice_pending = false;
            custom_sleep_image.reset();
            message = "休眠设置已保存";
            draw_settings();
            break;
        }
        case StorageSettings:
            message.clear(); draw_storage();
            break;
        case ClearCache:
            if (!bookmark_io_ready()) return;
            if (cancel_custom_preparation()) {
                clear_cache_pending = true;
                message = "正在停止图片准备，完成后清理缓存";
                draw_storage();
                break;
            }
            clear_cache_pending = false;
            abandon_content(); foreground_job.reset(); stop_covers(); clear_prefetch(); stop_counting_job();
            covers.clear(); custom_prepare_finished = false; custom_sleep_image.reset();
            message = store.clear_cache() == StoreStatus::Ok ? "缓存已清理，原书与阅读记录保留" : "缓存未完全清理，请检查 TF 卡";
            draw_storage();
            break;
        case InputSettings:
            message.clear();
            draw_input_settings();
            break;
        case SyncSettings:
            if (view == View::WifiSettings) leave_join(WifiPage::Home);
            account_from_sync = false;
            open_account();
            draw_sync_settings();
            break;
        case LibraryView: {
            if (!begin_settings_edit(true)) return;
            auto values = edit_snapshot.config.settings;
            const std::string previous = values.library_view == "grid" ? "卡片" : "列表";
            values.library_view = values.library_view == "grid" ? "list" : "grid";
            if (!bookmark_io_ready()) return;
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
        case WifiToggle: {
            if (!begin_settings_edit(true)) return;
            auto values = edit_snapshot.global_settings;
            values.wifi_enabled = !values.wifi_enabled;
            std::string detail;
            if (store.apply_settings(edit_snapshot, values, false, detail) != StoreStatus::Ok)
                wifi_notice = "Wi-Fi 开关未保存，仍按原设置生效。请检查存储卡。";
            else {
                settings.wifi_enabled = values.wifi_enabled;
                if (!values.wifi_enabled) {
                    cancel_sync(); network::release(manual_hold); network::release(wifi_hold);
                    if (wifi_status.phase != WifiPhase::Offline) wifi_status.phase = WifiPhase::Closing;
                }
                wifi_notice.clear();
            }
            draw_wifi();
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
            if (!bookmark_io_ready()) return;
            refresh_badge_config();
            cancel_sync();
            sync_status.clear();
            draw_sync();
            break;
        case SyncCheck:
            check_sync();
            return;
        case SyncUpload:
            if (!network::live(sync_hold)) { check_sync(); return; }
            confirm_upload();
            overlay();
            return;
        case SyncConfirm:
            if (!network::live(sync_hold)) { check_sync(); return; }
            upload();
            return;
        case SyncJump: {
            if (!remote_anchor)
                return;
            sync::Config current;
            std::string detail;
            if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
                cancel_sync();
                sync_state = SyncState::ConfigChanged;
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
        case ResumeDetails:
            inspect_book(has_book() ? book_path : last_book); if (!display.busy()) overlay(); return;
        case HelpPrevious: case HelpTools: case HelpNext: case HelpPosition: case HelpDevice:
            help_zone = id;
            if (id == HelpPosition) help_position = (help_position + 1) % 4;
            if (id == HelpDevice) help_device = 1 - help_device;
            draw_help(); break;
        case HelpKey1: case HelpKey2: case HelpKey3:
            help_key = id; draw_help(); break;
        case FullRefresh:
            force_refresh();
            return;
        case Prev:
        case Next:
            if (view == View::LockScreens) {
                if (id == Prev && lock_screen_page) --lock_screen_page;
                else if (id == Next) ++lock_screen_page;
                draw_lock_screens();
            } else if (view == View::Help) {
                if (id == Prev && help_page) --help_page;
                else if (id == Next && help_page + 1 < kHelpPages.size()) ++help_page;
                else return;
                draw_help();
            } else if (view == View::Library) {
                if (id == Prev && library_page)
                    --library_page;
                else if (id == Next && library_page + 1 < library_pages())
                    ++library_page;
                else return;
                draw_library();
            } else if (view == View::WifiSettings) {
                if (wifi_page != WifiPage::Add) return;
                if (id == Prev && wifi_add_page) --wifi_add_page;
                else if (id == Next) ++wifi_add_page;  // draw_wifi clamps to the last screen.
                draw_wifi();
            } else if (view == View::Contents && contents_bookmarks) {
                if (id == Prev && bookmark_page) --bookmark_page;
                else if (id == Next) ++bookmark_page;  // draw_bookmarks clamps to the last screen.
                draw_contents();
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
        case FontWeight:
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
                n = std::clamp(n + direction, 0, static_cast<int>(fonts.size()) - 1);
                draft.font = fonts[n];
                draft.font_weight = "regular";
            } else if (parameter == FontWeight) {
                const std::array<std::string, 4> weights{{"light", "regular", "medium", "bold"}};
                const auto at = std::find(weights.begin(), weights.end(), draft.font_weight);
                const int first = light_available(draft) ? 0 : 1;
                draft.font_weight = weights[std::clamp(static_cast<int>(at - weights.begin()) + direction, first, 3)];
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
            if (draft.font_weight == "light" && !light_available(draft)) draft.font_weight = "regular";
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
        case View::Help:
        case View::Update:
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
        // Key taps change the entry model only, so they are kept while a frame is in flight.
        if (accepted && view == View::TextEntry && key_input(input)) return;
        if (!accepted || display.busy() || view == View::Sleeping || view == View::PowerOff)
            return;
        if (async_content) {
            if (counting_job) counting_job->cancel();
        } else release_counting();
        if (content_loading) {
            if (!input.touch && input.event.input == Input::K2) force_refresh();
            else if (!input.queued && input.touch && loading_cancel.contains(input.x, input.y)) {
                const auto& request = pending_content ? *pending_content : foreground_job->request;
                const auto kind = request.kind;
                const bool preview = request.preview;
                const bool mapping = kind == ContentJob::Kind::MapUpload || kind == ContentJob::Kind::MapDownload;
                abandon_content();
                if (kind == ContentJob::Kind::ImageViewer) { close_image_viewer(); return; }
                link_return = confirmed_link_return;
                if (kind == ContentJob::Kind::LockScreenList) draw_power();
                else if (mapping) { sync_cancelled(false); draw_sync(); }
                else if (preview) draw_tools();
                else draw_library();
                overlay();
            }
            return;
        }
        if (image_viewing && view == View::Reading) {
            if (!input.queued && !input.touch && input.event.input == Input::K2) force_refresh();
            else if (!input.queued && input.touch && input.y < viewer_toolbar_height) {
                if (input.x >= viewer_action_left) {
                    image_contrast_level = (image_contrast_level + 1) % (viewer_contrast_levels + 1);
                    draw_image_viewer(true);
                } else close_image_viewer();
            }
            return;
        }
        const bool preserve_prefetch = view == View::Reading &&
            ((!input.touch && (input.event.input == Input::K3 || input.event.input == Input::Next || input.event.input == Input::K2)) ||
             (input.touch && input.x >= 479 && input.y < footer_top));
        if (!preserve_prefetch) clear_prefetch();
        if (input.queued) {
            const bool turn_touch = input.touch && input.y < footer_top && (input.x < 205 || input.x >= 479);
            const auto action = input.touch ? Action::None : MapInput(context(), input.event.input);
            const bool turn_key = action == Action::PreviousPage || action == Action::NextPage;
            if (view != View::Reading || !turned || (!turn_touch && !turn_key))
                return;
            // Links are not followed: the contact was aimed at the page that has been replaced.
            turn(turn_touch ? input.x >= 479 : action == Action::NextPage);
            return;
        }
        turned = false;
        // A notice lasts until the input after the one that showed it; K2 only repaints the page.
        if (view != View::Import && view != View::WifiSettings && view != View::Portal &&
            (input.touch || input.event.input != Input::K2) && std::exchange(notice_shown, false)) message.clear();
        if (input.touch) {
            if (view == View::Reading) {
                if (input.y >= footer_top) {
                    const bool device = input.x >= reading_footer::device_left;
                    if (input.touch_bounds &&
                        (input.touch_bounds->min_y < footer_top ||
                         (input.touch_bounds->min_x >= reading_footer::device_left) != device ||
                         (input.touch_bounds->max_x >= reading_footer::device_left) != device)) return;
                    if (device) {
                        footer_device = footer_device == reading_footer::Device::Battery
                                            ? reading_footer::Device::Clock : reading_footer::Device::Battery;
                    } else {
                        switch (footer_position) {
                        case reading_footer::Position::Pages: footer_position = reading_footer::Position::Percent; break;
                        case reading_footer::Position::Chapters: footer_position = reading_footer::Position::ChapterName; break;
                        case reading_footer::Position::ChapterName: footer_position = reading_footer::Position::Pages; break;
                        case reading_footer::Position::Percent: footer_position = reading_footer::Position::Chapters; break;
                        }
                    }
                    footer();
                    display.present({{"ui", 0, 0, ++ui_sequence}, {}, FramePurpose::Overlay,
                                     RefreshIntent::Content, RefreshRegion{0, footer_top, 684, 1216 - footer_top}});
                    return;
                }
                for (const auto &link : links)
                    if (link.contains(input.x, input.y)) {
                        control(link.id);
                        return;
                    }
                if (input.long_press)
                    if (const auto* box = image_at(input.x, input.y)) {
                        begin_image_viewer(*box);
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
                        // A power choice requires the whole sampled contact to stay in its button.
                        if (view == View::PowerMenu && input.touch_bounds) {
                            const auto& bounds = *input.touch_bounds;
                            if (!hit.contains(bounds.min_x, bounds.min_y) ||
                                !hit.contains(bounds.max_x, bounds.max_y)) return;
                        }
                        int id = hit.id;
                        // A long press on a book in the library opens its details instead of the book.
                        if (input.long_press && view == View::Library && id == Tools) id = ResumeDetails;
                        if (input.long_press && view == View::Library && id >= EntryBase && id < TocBase &&
                            !entries[static_cast<std::size_t>(id - EntryBase)].directory)
                            id += DetailBase - EntryBase;
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
    if (!bookmark_io_ready()) return;
    std::string detail;
    if (font_fallback || weight_unavailable) {
        error("所选字体或字重不可用，排版未保存");
        return;
    }
    const auto result = reset_overrides && book_scope
                            ? store.clear_book_settings(edit_snapshot, detail)
                            : store.apply_settings(edit_snapshot, draft, book_scope, detail);
    if (result != StoreStatus::Ok) {
        error("排版未保存。" + reason(detail) + "。");
        return;
    }
    const auto effective = store.load_settings(book_path).settings;
    const bool layout_changed = effective.font != settings.font ||
        effective.font_weight != settings.font_weight ||
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
    abandon_content();
    session.change_layout();
    show_page(offset);
}
void ReaderApp::Impl::apply_refresh(int id) {
    auto values = edit_snapshot.config.settings;
    values.refresh_preset = id == RefreshClear ? "clear" : id == RefreshFast ? "fast" : "balanced";
    save_refresh(values);
}
void ReaderApp::Impl::save_refresh(const ReaderSettings &values) {
    if (!bookmark_io_ready()) return;
    std::string detail;
    if (store.apply_settings(edit_snapshot, values, !refresh_from_settings, detail) != StoreStatus::Ok) {
        error("刷新设置未保存。" + reason(detail) + "。");
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
void ReaderApp::invalidate_identity_cache() { impl_->drain_save(); impl_->abandon_content(); impl_->foreground_job.reset(); impl_->stop_covers(); impl_->clear_prefetch(); impl_->stop_counting_job(); impl_->identities.clear(); impl_->font_identity.clear(); impl_->font_scan.reset(); impl_->effective_font.clear(); }
ReaderApp::~ReaderApp() { network::release(impl_->manual_hold); impl_->cancel_sync(); impl_->drain_save(); }
bool ReaderApp::initialize() {
#ifdef ESP_PLATFORM
    if (impl_->async_content && ContentJob::prepare_worker() != ContentJob::StartFailure::None)
        ESP_LOGW("reader_app", "Content worker reservation failed; content requests will retry");
#endif
    return impl_->initialize();
}
bool ReaderApp::open_book(const std::string &path) {
    if (impl_->portal_live || impl_->active_network || impl_->update_waiting()) return false;
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    return !impl_->usb_prepared && impl_->open(path);
}
void ReaderApp::set_battery(std::optional<int> percent, bool charging) {
    impl_->battery_percent = percent && *percent >= 0 && *percent <= 100 ? percent : std::nullopt;
    impl_->battery_charging = charging;
}
void ReaderApp::set_firmware_version(const std::string& version) { impl_->running_version = version; }
std::optional<UpdateOperation> ReaderApp::take_update_request() {
    if (impl_->display.busy() || impl_->update_worker || !impl_->update_request) return std::nullopt;
    impl_->update_worker = impl_->update_request->id;
    return std::exchange(impl_->update_request, std::nullopt);
}
void ReaderApp::update_progress(std::uint64_t id, std::uint32_t bytes) { impl_->update_progress(id, bytes); }
void ReaderApp::complete_update(std::uint64_t id, UpdateFailure failure, int status, const std::string& body,
                                const std::string& detail) {
    impl_->update_complete(id, failure, status, body, detail);
}
bool ReaderApp::update_busy() const { return impl_->update_waiting(); }
void ReaderApp::record_install() {
    impl_->update_state.pending_from = impl_->running_version;
    impl_->update_state.pending_to = impl_->update_to;
    impl_->save_update_state();
}
void ReaderApp::input(const AppInput &event) {
    impl_->input(event);
}
bool ReaderApp::can_long_press_at(int x, int y) const {
    if (impl_->image_at(x, y)) return true;
    return impl_->view == View::Library && std::any_of(impl_->hits.begin(), impl_->hits.end(), [&](const auto& hit) {
        return hit.contains(x, y) && (hit.id == Tools || (hit.id >= EntryBase && hit.id < TocBase &&
               !impl_->entries[static_cast<std::size_t>(hit.id - EntryBase)].directory));
    });
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
        if (impl_->view == View::Reading && !impl_->image_viewing && impl_->body_images_pending && !impl_->inline_image_pending)
            impl_->inline_image_after = std::chrono::steady_clock::now() + inline_image_delay;
        if (frame.refresh == RefreshIntent::FullClean)
            impl_->body_updates = 0;
        else if (frame.purpose == FramePurpose::Body)
            ++impl_->body_updates;
    }
    if (result && impl_->session.presented(*result)) {
        if (frame.purpose == FramePurpose::Body) impl_->remember_page();
        impl_->confirmed_link_return = impl_->link_return;
        impl_->save_failed = false;
        if (!impl_->async_content && !impl_->recent_save_failed && impl_->last_recorded != impl_->book_path) {
            std::string error;
            if (impl_->store.save_last_book(impl_->book_path, error) != StoreStatus::Ok) {
                impl_->recent_save_failed = true;
                impl_->message = "最近阅读记录未保存";
            } else
                impl_->last_recorded = impl_->book_path;
        }
    }
    if (success && impl_->view == View::Tools && std::exchange(impl_->tools_save_pending, false)) {
        impl_->schedule_save();
    }
    if (auto work = std::exchange(impl_->deferred, nullptr); work && success)
        work();
    // A final-frame failure must not restart application work during shutdown.
    if (impl_->view == View::PowerOff) return;
    if (!success) {
        impl_->abandon_content();
        if (impl_->usb_prepared) {
            impl_->message = "屏幕更新失败，请退出并重启。";
            impl_->draw_usb(); impl_->overlay(true);
        } else if (impl_->view == View::PowerMenu && impl_->power_return_canvas) {
            impl_->close_power_menu();
        } else if (frame.region) impl_->error("屏幕局部更新失败。返回将恢复已确认位置。");
        else impl_->error("屏幕更新失败。返回将恢复已确认位置。");
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
PageEffect ReaderApp::page_effect() const { return impl_->page_effect; }
void ReaderApp::set_page_effect(PageEffect effect) {
    impl_->page_effect = effect;
    if (impl_->view == View::PageEffects) {
        impl_->draw_page_effects();
        if (!impl_->display.busy()) impl_->overlay();
    }
}
int ReaderApp::auto_sleep_minutes() const {
    return impl_->settings.auto_sleep_minutes;
}
UsbTransferAction ReaderApp::take_usb_transfer_action() {
    if (impl_->usb_action == UsbTransferAction::Start) {
        if (impl_->usb_request_view && impl_->view != *impl_->usb_request_view) impl_->usb_action = UsbTransferAction::None;
        else if (impl_->cancel_custom_preparation()) return UsbTransferAction::None;
    }
    impl_->usb_request_view.reset();
    return std::exchange(impl_->usb_action, UsbTransferAction::None);
}
bool ReaderApp::prepare_usb_transfer() {
    if (impl_->cancel_custom_preparation()) return false;
    invalidate_identity_cache();
    if (impl_->network_worker_id || impl_->portal_live || impl_->usb_prepared || impl_->display.busy() || impl_->active_network ||
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
    impl_->overlay();
}
PortalAction ReaderApp::take_portal_action() {
    if (impl_->portal_action == PortalAction::Upload || impl_->portal_action == PortalAction::UploadStation) {
        if (impl_->view != impl_->portal_request_view) {
            impl_->portal_action = PortalAction::None;
            impl_->portal_requested = false;
        } else if (impl_->cancel_custom_preparation()) return PortalAction::None;
    }
    return std::exchange(impl_->portal_action, PortalAction::None);
}
std::uint64_t ReaderApp::portal_generation() const { return impl_->portal_generation; }
bool ReaderApp::wifi_enabled() const { return impl_->card_available && impl_->wifi_enabled(); }
void ReaderApp::stop_wifi(bool for_sleep) {
    if (!for_sleep) impl_->sleep_wifi_target.reset();
    else if (network::live(impl_->manual_hold) && impl_->manual_target)
        impl_->sleep_wifi_target = impl_->manual_target;
    network::release(impl_->manual_hold);
    network::release(impl_->sync_hold);
    network::release(impl_->wifi_hold);
    if (impl_->wifi_request) { impl_->wifi_request.reset(); impl_->wifi_worker = 0; }
    if (impl_->update_waiting()) {
        impl_->update_cancel();
        if (impl_->view == View::Update && !impl_->display.busy()) { impl_->draw_update(); impl_->overlay(); }
    }
    if (impl_->portal_live) impl_->portal_action = PortalAction::Stop;
    if (impl_->network_worker_id) {
        if (impl_->view != View::NetworkClosing) impl_->wait_network(SyncCancel);
        return;
    }
    const bool verifying = impl_->account_page == Impl::AccountPage::Verifying;
    impl_->cancel_sync();
    if (impl_->view == View::SyncSettings && !impl_->display.busy() && verifying) {
        impl_->account_notice = "Wi-Fi 已断开，账户没有保存。";
        impl_->draw_sync_settings(); impl_->overlay();
    }
    if (impl_->view == View::Sync && !impl_->display.busy() && impl_->sync_status != "Wi-Fi 已断开。") {
        impl_->sync_status = "Wi-Fi 已断开。";
        impl_->draw_sync(); impl_->overlay();
    }
    if (impl_->view == View::Portal) impl_->portal_action = PortalAction::Stop;
}
std::optional<WifiOperation> ReaderApp::take_wifi_request() {
    if (impl_->display.busy() || impl_->power_menu_wait ||
        impl_->view == View::PowerMenu || impl_->view == View::PowerOff) return std::nullopt;
    return std::exchange(impl_->wifi_request, std::nullopt);
}
void ReaderApp::complete_wifi(std::uint64_t id, const std::vector<provision::NearbyWifi>& nearby, const std::string& error,
                              LinkFailure failure) {
    if (!id || id != impl_->wifi_worker) return;
    impl_->wifi_worker = 0;
    switch (impl_->wifi_kind) {
    case WifiOperation::Kind::Scan:
        impl_->wifi_nearby = nearby;
        impl_->wifi_scanned = true;
        impl_->wifi_notice = error;
        break;
    case WifiOperation::Kind::Join:
        // A join that was cancelled after the link came up saves nothing.
        impl_->join_complete(error.empty() && !network::live(impl_->wifi_hold) ? "操作已取消。" : error, failure);
        network::release(impl_->wifi_hold);
        break;
    case WifiOperation::Kind::Connect:
        impl_->wifi_notice = error;
        if (!error.empty()) network::release(impl_->manual_hold);
    }
    if (impl_->view == View::WifiSettings && !impl_->display.busy()) { impl_->draw_wifi(); impl_->overlay(); }
}
void ReaderApp::show_wifi(const WifiStatus& status) {
    if (status.sequence <= impl_->wifi_status.sequence || impl_->display.busy()) return;
    impl_->wifi_status = status;
    if (status.phase == WifiPhase::Offline && !network::live(impl_->manual_hold) &&
        status.detail == "手动连接已暂停，点“立即连接”可恢复。") impl_->wifi_status.detail.clear();
    // A join saves its network, with the connection time, only when it completes; a cancelled or
    // failed join must leave network.toml as it was.
    if (status.phase == WifiPhase::Offline) impl_->join_link = false;
    if (status.phase == WifiPhase::Connected && !impl_->join_link) impl_->pending_wifi_record = status.ssid;
    if (status.phase == WifiPhase::Offline && impl_->wifi_notice == "正在结束…") impl_->wifi_notice.clear();
    // The join pages report their own result; link phases must not replace them.
    const bool joining = impl_->wifi_page == Impl::WifiPage::Joining || impl_->wifi_page == Impl::WifiPage::JoinFailed ||
                         impl_->wifi_page == Impl::WifiPage::Joined;
    if (impl_->view == View::WifiSettings && !joining) { impl_->draw_wifi(); impl_->overlay(); }
    // Sync retains one busy frame until completion; network phase notifications must not drive the panel during TLS.
}
bool ReaderApp::portal_visible() const {
    return impl_->portal_live && (impl_->view == View::Portal || impl_->view == View::WifiSettings);
}
WifiTargets ReaderApp::upload_targets() const {
    std::string problem;
    return impl_->card_available ? impl_->wifi_targets(problem) : WifiTargets{};
}
bool ReaderApp::keyboard_visible() const { return impl_->view == View::TextEntry; }
bool ReaderApp::prepare_portal(bool station) {
    if (impl_->cancel_custom_preparation()) return false;
    if (impl_->portal_live || impl_->network_worker_id) return false;
    if (impl_->content_loading || impl_->usb_prepared || impl_->display.busy() || impl_->active_network ||
        !impl_->card_available || !impl_->persist() || !wifi_enabled()) return false;
    impl_->cancel_sync();
    impl_->clear_body_cache();
    impl_->stop_covers();
    impl_->clear_prefetch();
    impl_->release_counting();
    impl_->stop_counting_job();
    if (!impl_->portal_requested) ++impl_->portal_generation;
    impl_->portal_requested = false;
    impl_->portal_live = true;
    impl_->portal_open_wifi = false;
    impl_->portal_station = station;
    if (!impl_->portal_station) impl_->manual_target.reset();
    impl_->portal_started = false;
    impl_->portal = {};
    impl_->portal_closing = false;
    impl_->draw_portal();
    impl_->overlay();
    return true;
}
void ReaderApp::show_portal(const PortalStatus &status) {
    if (impl_->usb_prepared || impl_->display.busy()) return;
    if (!status.generation || status.generation != impl_->portal_generation ||
        (!impl_->portal_live && !impl_->portal_requested)) return;
    const auto view = impl_->view;
    if (status.phase == PortalPhase::Closed) {
        const bool requested = impl_->portal_requested;
        impl_->portal_live = impl_->portal_requested = false;
        const bool open_wifi = std::exchange(impl_->portal_open_wifi, false) || view == View::WifiSettings;
        if (impl_->portal_started && impl_->card_available) {
            // The session may have added fonts and lock screen images. A selected font that was
            // missing may now exist, so the next load opens it instead of keeping the fallback.
            impl_->scan_fonts();
            impl_->effective_font.clear();
            impl_->custom_prepare_finished = false;
        }
        if (view != View::Portal && view != View::WifiSettings && (!requested || view != impl_->portal_request_view)) return;
        if (open_wifi) {
            impl_->refresh_badge_config();
            if (impl_->card_available) {
                invalidate_identity_cache(); impl_->scan_library();
            }
            impl_->portal = {};
            impl_->portal_closing = false;
            impl_->portal_station = impl_->portal_started = false;
            impl_->message = status.detail;
            impl_->wifi_notice = status.detail;
            impl_->wifi_page = impl_->wifi_root = Impl::WifiPage::Home;
            impl_->draw_wifi(); impl_->overlay(); return;
        }
        impl_->refresh_badge_config();
        impl_->portal = {};
        impl_->portal_closing = false;
        impl_->message = status.detail;
        const bool uploaded = view == View::Portal;
        const bool started = impl_->portal_started;
        const bool station_failed = uploaded && impl_->portal_station && !status.detail.empty();
        impl_->portal_station = impl_->portal_started = false;
        if (!uploaded || !started || station_failed) {
            if (started && impl_->card_available) {
                invalidate_identity_cache();
                impl_->scan_library();
            }
            impl_->draw_import();
        } else if (impl_->card_available) {
            invalidate_identity_cache();
            impl_->scan_library(); impl_->draw_library();
        } else impl_->draw_settings();
        impl_->overlay();
        return;
    }
    if (view != View::Portal && view != View::WifiSettings) return;
    // The codes must be scannable under every refresh preset, so their first frame clears the panel.
    const bool codes_appear = impl_->portal.url.empty() && !status.url.empty();
    impl_->portal_started = impl_->portal_started || status.phase == PortalPhase::Waiting;
    impl_->portal = status;
    if (view == View::WifiSettings) impl_->draw_wifi();
    else impl_->draw_portal();
    impl_->overlay(codes_appear);
}
bool ReaderApp::show_power_menu() {
    if (impl_->network_worker_id || impl_->display.busy() || impl_->active_network || impl_->content_loading || impl_->foreground_job ||
        impl_->usb_prepared || impl_->view == View::Portal || impl_->view == View::PowerOff) return false;
    if (impl_->view == View::PowerMenu) return true;
    cancel_sleep();
    impl_->control(PowerMenu);
    return impl_->view == View::PowerMenu || impl_->power_menu_wait.has_value();
}
PowerRequest ReaderApp::take_power_request() {
    return std::exchange(impl_->power_request, PowerRequest{});
}
void ReaderApp::power_unavailable(const std::string& message) {
    if (impl_->display.busy()) return;
    impl_->note(message, 1012);
    impl_->overlay();
}
bool ReaderApp::prepare_power(PowerRequest request) {
    const bool install = request.install && request.action == PowerAction::Restart && !request.without_save &&
        impl_->view == View::Update && impl_->update_page == Impl::UpdatePage::Ready && impl_->update_ready;
    if (request.action == PowerAction::None || request.install != install ||
        (!install && impl_->view != View::PowerMenu) ||
        request.action != impl_->power_selected || impl_->display.busy() || impl_->active_network ||
        impl_->usb_prepared || (request.without_save && !impl_->power_save_failed)) return false;
    bool saved = !request.without_save && impl_->persist();
    std::string detail;
    if (saved && impl_->has_book() && impl_->session.visible_record() && impl_->card_available) {
        saved = impl_->store.save_last_book(impl_->book_path, detail) == StoreStatus::Ok;
        if (saved) { impl_->last_recorded = impl_->book_path; impl_->recent_save_failed = false; }
    }
    if (install) {
        if (const auto battery = impl_->update_battery_block(); !battery.empty()) {
            impl_->power_request = {}; impl_->power_selected = PowerAction::None;
            impl_->update_notice = battery;
            impl_->draw_update(); impl_->overlay();
            return false;
        }
        if (!saved) {
            impl_->power_request = {}; impl_->power_selected = PowerAction::None;
            impl_->update_notice = "阅读位置没有写入 TF 卡，没有重启。";
            impl_->draw_update(); impl_->overlay();
            return false;
        }
    }
    impl_->power_save_failed = !saved;
    impl_->power_progress_saved = saved;
    if (!saved && !request.without_save) {
        impl_->message = "阅读位置、书签或最近阅读记录未能保存到 TF 卡，仍要" +
            std::string(request.action == PowerAction::Restart ? "重启吗？" : "关机吗？");
        impl_->draw_power_menu(); impl_->overlay(); return false;
    }
    impl_->remember_position();
    impl_->cancel_sync();
    impl_->drain_save(); impl_->abandon_content(); impl_->foreground_job.reset();
    impl_->stop_covers(); impl_->clear_prefetch(); impl_->stop_counting_job();
    // Saving is the last cancellable step. Never restore a snapshot after teardown starts.
    impl_->power_return_canvas.reset(); impl_->power_return_hits.clear();
    impl_->power_request = {};
    if (install) impl_->draw_installing();
    else impl_->draw_power_off(saved ? "本地阅读位置已保存。" : "本地阅读位置未完整保存。");
    impl_->overlay(true);
    return true;
}
bool ReaderApp::close_power_files() {
    if (impl_->display.busy() || impl_->view != View::PowerOff) return false;
    impl_->document.close();
    impl_->ui.open_fallback(); impl_->body.open_fallback();
    impl_->identities.clear(); impl_->font_scan.reset(); impl_->font_identity.clear();
    return true;
}
void ReaderApp::show_power_off_help() {
    if (impl_->display.busy() || impl_->view != View::PowerOff) return;
    impl_->draw_power_off(std::string(impl_->power_progress_saved ? "本地阅读位置已保存。" : "本地阅读位置未完整保存。") +
        "请长按电源键强制关机；若连接 USB，请拔下电源线。", true);
    impl_->overlay(true);
}
bool ReaderApp::sleep_pending() const { return impl_->sleep_cover_deadline.has_value(); }
void ReaderApp::cancel_sleep() {
    impl_->custom_sleep_attempted = false;
    impl_->custom_prepare_finished = false;
    impl_->custom_sleep_image.reset();
    if (impl_->sleep_cover_deadline) {
        impl_->sleep_cover_deadline.reset();
        if (impl_->counting_job) impl_->counting_job->cancel();
    }
    if (const auto target = std::exchange(impl_->sleep_wifi_target, std::nullopt);
        target && impl_->card_available && impl_->wifi_enabled() && !impl_->usb_prepared && !impl_->portal_live) {
        // A released hold cannot be reused: the worker may already have completed its teardown.
        impl_->manual_hold = std::make_shared<std::atomic_bool>(false);
        impl_->wifi_worker = ++impl_->wifi_sequence;
        impl_->wifi_request = WifiOperation{WifiOperation::Kind::Connect, impl_->wifi_worker, *target, impl_->manual_hold};
    }
}
bool ReaderApp::prepare_sleep() {
    if (impl_->power_menu_wait) return false;
    bool cover_attempted = false;
    if (impl_->sleep_cover_deadline) {
        impl_->poll_counting_job();
        if (impl_->counting_job) {
            // Cancellation does not release files; completion must precede storage suspension.
            if (std::chrono::steady_clock::now() >= *impl_->sleep_cover_deadline)
                impl_->counting_job->cancel();
            return false;
        }
        impl_->sleep_cover_deadline.reset();
        cover_attempted = impl_->sleep_waiting_for_cover;
    }
    if (impl_->network_worker_id || impl_->portal_live || impl_->usb_prepared || impl_->view == View::Portal || impl_->view == View::PowerMenu || impl_->view == View::PowerOff) return false;
    if (impl_->display.busy() || impl_->active_network) return false;
    if (impl_->counting_job) {
        // Every background owner must return before cleanup or a sleep image can replace it.
        const auto kind = impl_->counting_job->request.kind;
        impl_->sleep_waiting_for_cover = kind == ContentJob::Kind::SleepCover;
        if (kind != ContentJob::Kind::SleepCover && kind != ContentJob::Kind::LockScreen)
            impl_->counting_job->cancel();
        impl_->sleep_cover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        return false;
    }
    invalidate_identity_cache();
    if (!impl_->persist()) return false;
    if (impl_->has_book() && !impl_->session.visible_record()) return false;
    impl_->remember_position();
    impl_->save_pages();
    impl_->clear_body_cache();
    impl_->clear_prefetch();
    impl_->release_counting();
    impl_->resume_to_library = impl_->view == View::Library || !impl_->has_book();
    if (impl_->settings.sleep_screen == "custom" && !impl_->custom_sleep_attempted) {
        impl_->prepare_custom_sleep(true);
        if (impl_->counting_job) {
            impl_->sleep_waiting_for_cover = false;
            impl_->sleep_cover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            return false;
        }
    }
    if (!cover_attempted && impl_->async_content && impl_->has_book() && !txt_name(impl_->book_path) &&
        impl_->settings.sleep_screen == "cover") {
        impl_->prepare_sleep_cover();
        if (impl_->counting_job) {
            impl_->sleep_waiting_for_cover = true;
            impl_->sleep_cover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            return false;
        }
    }
    const auto sync_state = impl_->sync_badge();
    impl_->cancel_sync();
    network::release(impl_->manual_hold);
    network::release(impl_->wifi_hold);
    // Password drafts never outlive the flow that entered them.
    impl_->leave_join(Impl::WifiPage::Home);
    std::fill(impl_->account.password.begin(), impl_->account.password.end(), '\0');
    impl_->account.password.clear();
    impl_->draw_sleep(sync_state);
    if (impl_->view != View::Sleeping) return false;
    impl_->overlay(true);
    return true;
}
void ReaderApp::wake(bool slept) {
    if (impl_->display.busy())
        return;
    impl_->wifi_status.phase = WifiPhase::Offline;
    impl_->wifi_status.manual = false;
    impl_->wifi_status.ssid.clear(); impl_->wifi_status.detail.clear();
    impl_->wifi_notice.clear();
    impl_->custom_sleep_attempted = false;
    impl_->custom_prepare_finished = false;
    impl_->custom_notice_pending = slept && impl_->settings.sleep_screen == "custom" && !impl_->custom_notice.empty();
    if (slept) impl_->sleep_wifi_target.reset();
    else cancel_sleep();
    if (!impl_->resume_to_library && impl_->has_book()) {
        if (impl_->async_content) impl_->open(impl_->book_path);
        else impl_->recover_body();
    } else {
        impl_->draw_library();
        impl_->overlay(true);
    }
}
void ReaderApp::card_removed() {
    stop_wifi();
    impl_->power_return_canvas.reset(); impl_->power_request = {};
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
    return !impl_->portal_live && impl_->view != View::PowerMenu && impl_->view != View::PowerOff &&
           !impl_->usb_prepared && !impl_->display.busy() && (impl_->session.needs_save() ||
           (impl_->async_content && !impl_->recent_save_failed && impl_->session.visible_record() && impl_->last_recorded != impl_->book_path)) &&
           !impl_->save_failed && !impl_->save_job;
}
void ReaderApp::save_progress() {
    if (impl_->portal_live || impl_->usb_prepared || impl_->display.busy()) return;
    impl_->schedule_save();
}
bool ReaderApp::save_busy() const { return impl_->save_job || impl_->bookmark_job; }
std::optional<BookPages> ReaderApp::book_pages() const {
    const auto page = impl_->has_book() ? impl_->current_page() : 0;
    return page ? std::optional<BookPages>{{page, impl_->total_pages()}} : std::nullopt;
}
CacheUsage ReaderApp::cache_usage() const { return impl_->cache_usage(); }

bool ReaderApp::content_busy() const {
    return !!impl_->foreground_job || impl_->pending_content.has_value() ||
           (impl_->async_content && impl_->view == View::Reading && !impl_->image_viewing && impl_->body_images_pending);
}
bool ReaderApp::background_due() const {
    return !!impl_->cover_job || impl_->pending_covers.has_value() || !!impl_->counting_job ||
           impl_->custom_prepare_due() || impl_->prefetch_due() || impl_->counting_due();
}
#if !defined(ESP_PLATFORM) || defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
// Only the diagnostic console reads this; other firmware builds do not carry the name table.
const char *ReaderApp::sync_state() const {
    static constexpr const char *names[] = {"unchecked", "no-record", "same", "different", "unmapped", "uploaded",
                                            "uncertain", "unauthorized", "failed", "link-failed", "config-changed"};
    return names[static_cast<unsigned>(impl_->sync_state)];
}
int ReaderApp::sync_order() const { return impl_->sync_state == Impl::SyncState::Different ? impl_->further() : 0; }
int ReaderApp::font_size_px() const { return impl_->settings.font_size_px; }
#endif
const char *ReaderApp::background_state() const {
    if (impl_->inline_image_pending || (impl_->view == View::Reading && !impl_->image_viewing && impl_->body_images_pending)) return "image";
    if (impl_->content_loading) {
        const auto kind = impl_->pending_content ? impl_->pending_content->kind : impl_->foreground_job->request.kind;
        return kind == ContentJob::Kind::Open ? "opening" : kind == ContentJob::Kind::Inspect ? "details" :
            kind == ContentJob::Kind::MapUpload || kind == ContentJob::Kind::MapDownload ? "mapping" :
            kind == ContentJob::Kind::Chapter ? "chapter" : "image";
    }
    if (impl_->cover_job || impl_->pending_covers) return "covers";
    if (impl_->counting_job && (impl_->counting_job->request.kind == ContentJob::Kind::SleepCover || impl_->counting_job->request.kind == ContentJob::Kind::LockScreen)) return "sleep-cover";
    if (impl_->prefetch_due()) return "prefetch";
    if (impl_->counting_blocked == impl_->chapter_index) return "waiting";
    if ((!impl_->pages_cache_disabled && impl_->pages_complete && impl_->font_identity.empty()) ||
        (impl_->counting_job && impl_->counting_job->request.kind == ContentJob::Kind::FontIdentity))
        return "font-identity";
    if (!impl_->pages_cache_disabled && !impl_->font_identity.empty() &&
        (impl_->pages_cache_pending || impl_->pages_dirty)) return "page-cache";
    if (!impl_->has_book() || impl_->pages_complete) return "idle";
    if (impl_->pages_failed) return "failed";
    return "counting";
}
void ReaderApp::poll_background(bool cancel) {
    if (impl_->view == View::PowerOff) return;
    if (cancel && impl_->body_images_pending && !impl_->inline_image_pending && !impl_->display.busy())
        impl_->inline_image_after = std::chrono::steady_clock::now() + inline_image_delay;
    if (cancel && impl_->counting_job) impl_->counting_job->cancel();
    impl_->poll_counting_job();
    if (impl_->clear_cache_pending) {
        if (impl_->view != View::StorageSettings || !impl_->card_available) impl_->clear_cache_pending = false;
        else if (!cancel && !impl_->counting_job && !impl_->display.busy() && !impl_->save_job && !impl_->bookmark_job)
            impl_->control(ClearCache);
    }
    // The counting session lives only while the reading page is shown.
    if (impl_->view != View::Reading && !impl_->counting_job) impl_->counting_session.reset();
    impl_->poll_covers();
    impl_->poll_save();
    impl_->poll_bookmark();
    if (!cancel) impl_->reclaim_caches();
    if (impl_->save_error_pending && impl_->view != View::PowerMenu && !impl_->display.busy() && !impl_->content_loading) {
        impl_->save_error_pending = false; impl_->error(impl_->message);
    }
    if (!impl_->bookmark_error.empty() && impl_->view != View::PowerMenu && impl_->view != View::PowerOff &&
        !impl_->display.busy() && !impl_->content_loading) {
        impl_->error(std::exchange(impl_->bookmark_error, std::string()));
    }
    if (!cancel || !impl_->content_loading) impl_->poll_content(!cancel);
    // Waits for the queued taps of one burst, so they reach the panel in a single update.
    if (!cancel) impl_->flush_entry();
    if (impl_->custom_notice_pending && !impl_->display.busy() && !impl_->content_loading && !impl_->foreground_job &&
        (impl_->view == View::Library || impl_->view == View::Reading)) {
        impl_->custom_notice_pending = false;
        impl_->error(impl_->custom_notice + "。请检查 LockScreens/ 图片；不可用时使用简洁屏保。");
    }
    if (!cancel && !impl_->pending_wifi_record.empty() && impl_->card_available &&
        !impl_->portal_live && !impl_->usb_prepared && !impl_->network_worker_id &&
        !impl_->display.busy() && !impl_->save_job && !impl_->bookmark_job && !impl_->foreground_job) {
        std::string error;
        const auto name = std::exchange(impl_->pending_wifi_record, std::string());
        if (!provision::save_connection(impl_->root, name, std::time(nullptr), error))
            impl_->wifi_notice = "连接记录未保存，请检查 TF 卡。";
    }
    if (impl_->power_menu_wait) {
        if (impl_->view != *impl_->power_menu_wait || !impl_->card_available) impl_->power_menu_wait.reset();
        else if (!cancel && !impl_->counting_job && !impl_->display.busy() && !impl_->save_job &&
                 !impl_->bookmark_job && !impl_->foreground_job && !impl_->content_loading) {
            impl_->power_menu_wait.reset();
            show_power_menu();
        }
    }
    // Only prepare images after the displayed page has remained idle; panels may restore an unfinished page.
    if (!cancel && !impl_->sleep_cover_deadline && !impl_->power_menu_wait && impl_->async_content && impl_->body_images_pending && !impl_->inline_image_pending &&
        std::chrono::steady_clock::now() >= impl_->inline_image_after &&
        !impl_->content_loading && !impl_->foreground_job && !impl_->pending_content &&
        impl_->view == View::Reading && !impl_->image_viewing && !impl_->display.busy() &&
        impl_->has_book() && impl_->card_available && !impl_->usb_prepared && !impl_->page.images.empty())
        impl_->begin_image(impl_->page.start, impl_->page, false, true);
}
void ReaderApp::background_step(const std::function<bool()>& stop) {
    if (impl_->view == View::PowerOff) return;
    impl_->poll_counting_job();
    if (stop && stop()) { if (impl_->counting_job) impl_->counting_job->cancel(); return; }
    if (impl_->custom_prepare_due()) impl_->prepare_custom_sleep(false);
    else if (impl_->prefetch_due()) impl_->prepare_next(stop);
    else if (impl_->counting_due()) impl_->count_pages(stop);
}
void ReaderApp::show_error(const std::string &message) {
    if (!impl_->display.busy())
        impl_->error(message);
}
std::optional<NetworkOperation> ReaderApp::take_network_request() {
    if (impl_->display.busy())
        return std::nullopt;
    const bool account = impl_->view == View::SyncSettings && impl_->account_check;
    if (impl_->network_worker_id || (impl_->view != View::Sync && !account) || !impl_->active_network ||
        !impl_->network_request || impl_->network_request->id != impl_->active_network)
        return std::nullopt;
    if (!wifi_enabled()) {
        impl_->cancel_sync();
        if (account) impl_->draw_sync_settings(); else impl_->draw_sync();
        impl_->overlay();
        return std::nullopt;
    }
    impl_->network_worker_id = impl_->active_network;
    auto request = std::move(impl_->network_request);
    impl_->network_request.reset();
    return request;
}
void ReaderApp::complete_network(std::uint64_t id, int status, const std::string &body,
                                 const std::string &error, LinkFailure failure) {
    if (id == impl_->network_worker_id && id) {
        impl_->network_worker_id = 0;
        if (impl_->network_after_cancel) {
            const int action = std::exchange(impl_->network_after_cancel, 0);
            if (impl_->view == View::NetworkClosing) {
                impl_->view = impl_->network_return_view;
                if (impl_->view == View::SyncSettings) { impl_->draw_sync_settings(); impl_->overlay(); }
                else if (action == SyncCancel) { impl_->draw_sync(); impl_->overlay(); }
                else impl_->control(action);
            }
            return;
        }
    }
    if (!impl_->usb_prepared) impl_->network_complete(id, status, body, error, failure);
}
} // namespace pico_page
