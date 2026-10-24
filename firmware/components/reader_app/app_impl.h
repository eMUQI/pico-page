#pragma once

// Private application state and shared inline helpers. Page implementations use the same
// owner; splitting translation units does not introduce additional tasks or state copies.

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
#include <cstdlib>
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
namespace app_detail {
using graphics::decode_utf8;
inline constexpr int footer_top = 1128, toolbar_top = 1080;
inline constexpr std::size_t kSleepWaitFooterBytes = 342U * (1216U - footer_top);
inline constexpr char kProjectHome[] = "picopage.wulu.zone";
inline constexpr int viewer_left = 24, viewer_top = 104, viewer_width = 636, viewer_height = 1088;
inline constexpr int viewer_toolbar_height = 96, viewer_action_left = 456;
inline constexpr int viewer_contrast_levels = 3;
inline constexpr auto inline_image_delay = std::chrono::milliseconds(250);
inline constexpr const char* kSleepFallbackNotice = "本次使用简洁屏保";
inline constexpr const char *content_revision = "epub-native-v1";
inline constexpr const char *txt_revision = "txt-utf8-v1";
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
    PageEffects, EffectOff, EffectRipple,
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
    WifiClock, WifiClockCancel,
    WifiSavedBase = 50100, WifiModeBase = 50200, WifiNearbyBase = 50300,
    SleepCustom = 51000, SleepRotation, SleepChoose, SleepRefresh, SleepOverlay, SleepPreview, LockScreenBase = 52000,
    UpdateOpen = 53000, UpdateCheck, UpdateNotes, UpdateStart, UpdateConfirm, UpdateCancel,
    UpdateRetry, UpdateLater, UpdateRestart, UpdateDismiss,
    FontEntryBase = 54000, FontAccept = 54300, FontCancel,
    DictionaryOpen = 55000, DictionaryScan, DictionaryClose, DictionaryExpand, DictionaryRetry,
    DictionarySources, DictionaryNextEntry, DictionaryEdit, DictionaryStart, DictionaryEnd, DictionaryManage,
    DictionaryToggleBase = 56000, DictionaryUpBase = 56100, DictionaryDownBase = 56200, DictionaryPrepareBase = 56300, DictionarySourceBase = 56400
};
static_assert(ContentsBookmarks < EntryBase);
inline std::string basename(const std::string &path) {
    const auto i = path.find_last_of('/');
    return path.substr(i == std::string::npos ? 0 : i + 1);
}
inline bool relative_path(const std::string &value) {
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
inline bool safe_existing(const std::string &root, const std::string &relative) {
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
inline bool txt_name(std::string name) {
    for (auto &c : name)
        if (c >= 'A' && c <= 'Z') c += 32;
    return name.size() > 4 && name.substr(name.size() - 4) == ".txt";
}
inline bool book_name(std::string name) {
    for (auto &c : name)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return (name.size() > 5 && name.substr(name.size() - 5) == ".epub") ||
           (name.size() > 4 && name.substr(name.size() - 4) == ".txt");
}
inline const char* revision_for(const std::string& path, const DocumentMetadata& metadata) {
    return txt_name(path) ? (metadata.txt_gb18030 ? kTxtGb18030Revision : txt_revision) : content_revision;
}
/**
 * Single-line display text of at most limit scalars: control and line-separator characters
 * become one space, runs of spaces collapse, and surrogates or out-of-range values are dropped.
 */
inline std::string snippet(std::u32string_view text, std::size_t limit) {
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
} // namespace app_detail
using namespace app_detail;

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
    void prepare_next(const std::function<bool()>& stop);
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
    enum class Origin : std::uint8_t { Reading, Library, Settings };
    Origin error_origin = Origin::Library, sleep_origin = Origin::Library, loading_origin = Origin::Library;
    bool sleep_destination_captured = false;
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
    void poll_covers();
    std::vector<Hit> hits, links;
    std::optional<Anchor> link_return, confirmed_link_return;
    bool sync_return = false, confirmed_sync_return = false;
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
    void toggle_bookmark();
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
    bool finish_bookmark(bool redraw);
    // A single configured root is presented as the library home instead of as a folder entry.
    std::string library_home_root;
    // The open folder is one of the configured library roots; its parent is the library.
    bool folder_root = false;
    bool library_directory_unavailable = false;
    reading_footer::Position footer_position = reading_footer::Position::Pages;
    reading_footer::Device footer_device = reading_footer::Device::Battery;
    bool footer_charging = false;
    PageEffect page_effect = PageEffect::Off;
    bool refresh_from_settings = false, import_from_settings = false, help_from_settings = false, typography_from_settings = false;
    Origin origin_for(View source) const;
    static const char *return_label(Origin origin) {
        switch (origin) {
        case Origin::Reading: return "恢复阅读";
        case Origin::Library: return "返回书库";
        case Origin::Settings: return "返回设置";
        }
        return "返回书库";
    }
    void return_to_origin(Origin origin, bool full = false) {
        if (origin == Origin::Reading && has_book()) { recover_body(); return; }
        if (origin == Origin::Settings) {
            draft = {}; edit_snapshot = {}; reset_overrides = false;
            leave_join(WifiPage::Home);
            std::fill(account.password.begin(), account.password.end(), '\0');
            account = {}; entry = {};
            draw_settings();
        } else draw_library();
        overlay(full);
    }
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
    static constexpr std::size_t fonts_per_screen = 4;
    bool font_picker_open = false, font_entry_reset = false;
    std::size_t font_picker_page = 0;
    std::string font_entry, font_entry_weight;
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
    std::shared_ptr<DictionarySession> dictionary_session;
    std::vector<DictionaryItem> dictionary_items;
    std::vector<std::string> dictionary_ready;
    std::size_t dictionary_list_page = 0, dictionary_source = 0, dictionary_entry = 0, dictionary_entry_base = 0;
    bool dictionary_list_complete = true, dictionary_full = false, dictionary_inflections = false, dictionary_search_all = true;
    bool dictionary_partial_error = false, dictionary_missing_glyphs = false, dictionary_omitted_fields = false;
    std::size_t dictionary_candidate_length = 0;
    std::size_t dictionary_layout_anchor = 0, dictionary_picker_page = 0;
    bool dictionary_choosing = false, dictionary_settings_from_lookup = false, dictionary_settings_changed = false;
    std::string dictionary_return_notice, dictionary_settings_source;
    int dictionary_page_move = 0;
    int dictionary_touch_y = 0;
    std::string dictionary_notice, dictionary_headword, dictionary_name, dictionary_matched_word;
    dictionary::Selection dictionary_selection;
    dictionary::Selection dictionary_contact_anchor;
    std::uint64_t dictionary_contact = 0;
    std::size_t dictionary_trigger = 0;
    int dictionary_drag_dx = 0, dictionary_drag_dy = 0;
    bool dictionary_selecting = false, dictionary_manual = false;
    bool dictionary_selection_dirty = false, dictionary_query_pending = false, dictionary_close_pending = false;
    std::vector<dictionary::Candidate> dictionary_candidates;
    std::vector<dictionary::Entry> dictionary_entries;
    bool dictionary_more_entries = false, dictionary_page_end = true;
    std::shared_ptr<const std::vector<dictionary::Field>> dictionary_definition;
    std::vector<dictionary::TextCursor> dictionary_pages;
    dictionary::TextCursor dictionary_next;
    std::uint32_t dictionary_page_number = 0;
    graphics::Page dictionary_page;
    void queue_dictionary(ContentJob::Request request);
    void open_dictionary_settings(bool from_lookup = false);
    void leave_dictionary_settings();
    void draw_dictionary_sources();
    void cancel_dictionary_sources();
    void select_dictionary(std::size_t source);
    void draw_dictionary_settings();
    void begin_dictionary(int x, int y, bool selecting = false);
    bool dictionary_contact_input(const AppInput& input, bool accepted);
    void submit_dictionary_selection();
    void flush_dictionary_selection();
    void reset_dictionary_contact();
    void query_dictionary();
    void page_dictionary(int movement = 0);
    void draw_dictionary();
    void finish_dictionary(ContentJob& job);
    void dictionary_control(int id);
    void close_dictionary();
    int dictionary_top(bool full) const;
    graphics::LayoutSpec dictionary_layout(bool full) const;
    ContentCacheEntry pending_chapter_cache;
    std::optional<std::chrono::steady_clock::time_point> chapter_feedback_after;
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
        pending_chapter_cache = {};
        chapter_feedback_after.reset();
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
        loading_origin = origin_for(view);
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
    void draw_image_viewer(bool full = false);
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
    bool begin_image(std::size_t start, const graphics::Page& planned, bool preview = false, bool progressive = false);
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
                       std::size_t position = 0, const Anchor& anchor = {}, const std::string& fragment = "");
    bool begin_seek(const Anchor& anchor) {
        std::size_t index;
        if (!document.chapter_index_for(anchor.resource, index)) { error("目标位置不在正文中"); return false; }
        return begin_chapter(index, 0, ContentJob::Target::Anchor, 0, anchor);
    }
    /** Applies only image pixels; text, footer, anchor and reading revision stay owned by the visible page. */
    void complete_inline_image(const ContentJob& job);
    void poll_content(bool allow_inline = true);

    void stop_counting_job() {
        sleep_cover_deadline.reset();
        if (counting_job) { counting_job->cancel(); counting_job.reset(); }
        counting_session.reset();
        font_scan.reset();
    }
    void poll_counting_job();
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
    void poll_save();
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
              std::uint8_t gray = 1, bool bold = false);
    void text(graphics::Font& font, const std::string& value, int x, int top, int size,
              int max_width, std::uint8_t gray, bool bold);
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
    /** Measures or draws the same bounded title layout, retaining closing punctuation with its preceding character. */
    int title_lines(const std::string &value, int x, int y, int width, int size, bool bold = false, int max_rows = 2,
                    std::uint8_t gray = 1, bool draw = true);
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
    std::pair<std::uint64_t, std::uint64_t> estimated_position(std::size_t index, const sync::PartPosition &at) const;
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
        case reading_footer::Position::Hidden:
            return {};
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
    void reading_header(bool panel = false);
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
    void tool_icon(int id, int cx, int cy);
    void note(const std::string &value, int y = 1020) {
        text(value, 36, y, 24, 612, 3);
    }
    void force_refresh() {
        const auto &visible = session.visible_record();
        display.present({{"ui", 0, 0, ++ui_sequence}, visible ? visible->anchor : Anchor{},
                         FramePurpose::ManualRefresh, RefreshIntent::FullClean});
    }
    /** Submits a whole-screen UI update; Display holds equal pixels unless a full clean is required. */
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
    void load_fonts(const ReaderSettings *preview = nullptr);
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
    void scan_library();
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
    void describe(Entry &entry);
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
                     int width = kLibraryCoverWidth, int height = kLibraryCoverHeight);
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
    void draw_library(bool decode = false);
    bool open(std::string path);
    bool finish_open(const std::string& path, ContentJob::Result* loaded = nullptr);
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
    void footer();
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
        pages_header = std::string("PPPAGES3\n") + revision_for(book_path, document.metadata()) + ":pagination-v4\n" + book_path + "\n" + font_file + "\n" + geometry +
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
        usage.cover_pixels += sleep_wait_footer ? kSleepWaitFooterBytes : 0;
        usage.ui_glyph_bytes = ui.cached_glyph_bytes(); usage.body_glyph_bytes = body.cached_glyph_bytes();
        usage.ui_glyphs = ui.cached_glyphs(); usage.body_glyphs = body.cached_glyphs();
        usage.ui_advances = ui.cached_advances(); usage.body_advances = body.cached_advances();
        usage.evictions = cache_evictions; usage.target = cache_target;
        usage.retained = pending_chapter_cache.bytes.capacity() + usage.body_bitmap + usage.next_layout + usage.next_bitmap +
            usage.page_index + usage.cover_pixels + usage.ui_glyph_bytes + usage.body_glyph_bytes +
            (usage.ui_advances + usage.body_advances) * (sizeof(std::uint64_t) + sizeof(int));
        return usage;
    }
    void reclaim_caches();
    bool background_content_ready() const {
        return !sleep_cover_deadline && !power_menu_wait && !session.needs_save() && view == View::Reading && !image_viewing && has_book() && card_available && !usb_prepared &&
               !display.busy() && !deferred && !save_job && !bookmark_job && !foreground_job && !pending_content && !body_images_pending &&
               !cover_job && !pending_covers;
    }
    bool chapter_cache_due() const {
        return !pending_chapter_cache.bytes.empty() && background_content_ready() && !counting_job;
    }
    void write_chapter_cache() {
        ContentJob::Request request;
        request.kind = ContentJob::Kind::ContentCacheWrite;
        request.root = root; request.generation = counting_generation;
        request.content_cache = std::move(pending_chapter_cache);
        pending_chapter_cache = {};
        counting_job = std::make_unique<ContentJob>(std::move(request));
        if (!counting_job->start()) counting_job.reset();
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
    void count_pages(const std::function<bool()>& stop);
    void draw_image_error(const graphics::ImageBox& box) {
        outline(box.x, box.y + 12, box.width, box.height - 24, 2);
        center("图片无法显示", box.x + 8, box.y + box.height / 2 - 36, box.width - 16, 28);
        center("图片缺失、损坏或内存不足", box.x + 8, box.y + box.height / 2 + 16, box.width - 16, 22, 3);
    }
    bool draw_page_content(const graphics::Page& planned, const graphics::LayoutSpec& spec, bool progressive = false);
    bool body_reusable(std::size_t start, const graphics::LayoutSpec& spec) const {
        return body_canvas && start == page.start && same_layout(body_spec, spec);
    }
    bool render_body(std::size_t start);
    bool show_page(std::size_t start, bool reverse_page_effect = false);
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
        show_page(previous, true);
    }
    /**
     * Shows the preceding page. Cache misses over long text ranges are announced before
     * pagination because the first measurements may read glyph metrics from the card.
     */
    void turn_back_to(std::size_t limit) {
        if (const auto cached = previous_page(limit)) { show_page(*cached, true); return; }
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
    void turn(bool forward);
    /** Removes the sync return bar from a turn candidate; failure restores the confirmed state. */
    void expire_sync_return() {
        if (!sync_return) return;
        link_return.reset();
        sync_return = false;
    }
    void draw_tools();
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
    void draw_bookmarks(int list_top);
    void draw_contents();
    int typography_preview_bottom() const { return font_picker_open ? 500 : preview_bottom; }
    void draw_font_picker();
    void cancel_font_picker() {
        draft.font = font_entry;
        draft.font_weight = font_entry_weight;
        reset_overrides = font_entry_reset;
        font_picker_open = false;
        draw_typography();
    }
    void font_picker_control(int id);
    void draw_typography();
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
    /** Two decimals without trailing zeros: 1.50 reads 1.5. */
    static std::string compact(double value) {
        auto shown = decimal(value);
        shown.erase(shown.find_last_not_of('0') + 1);
        if (shown.back() == '.') shown.pop_back();
        return shown;
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
            header("阅读", Back, "设置");
        }
        canvas.fill_rect(0, top, 684, 1216 - top, 15);
        return true;
    }
    /** The settings page for reading defaults: default typography and the refresh controls. */
    void draw_reading_settings();
    void draw_refresh() {
        if (refresh_from_settings) { draw_reading_settings(); return; }
        view = View::Refresh;
        if (!panel_background(502 - panel_strip)) return;
        tabs(502, Refresh);
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
        hits.clear(); canvas.clear(); header("翻页效果", Back, refresh_from_settings ? "阅读" : "刷新");
        text("实验功能 · 本次启动有效", 36, 144, 26, 612, 3);
        choice(EffectOff, "关闭", "沿用当前刷新模式", 220, page_effect == PageEffect::Off);
        choice(EffectRipple, "水波纹", "条带依次展开，翻页可能更慢", 356, page_effect == PageEffect::Ripple);
        wrapped("仅用于正文非全刷更新。清晰模式每页全刷，不播放效果。", 36, 936, 612, 25, 3);
        wrapped("效果、残影和灰阶均待真机验证。按全刷键可清理当前画面。", 36, 1040, 612, 25, 3);
    }
    void draw_refresh_confirm() {
        draw_refresh();
        if (view == View::Error) return;
        view = View::RefreshConfirm;
        hits.clear();
        if (has_book() && !refresh_from_settings) header(document.metadata().title, 0);
        else header("阅读", 0);
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
        hits.clear(); canvas.clear(); header("自动全刷间隔", Back, refresh_from_settings ? "阅读" : "刷新");
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
        canvas.line(36, y + height - 1, 647, y + height - 1, rule, 2);
        if (!id) return;
        text("›", 636, y + 42 - lift, 28, 24);
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
    void draw_wifi();
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
    std::vector<Key> keyboard() const;
    void draw_entry();
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
    bool key_input(const AppInput& input);
    /** Batches key taps into one regional Overlay; Display holds equal field and key pixels. */
    void flush_entry() {
        if (view != View::TextEntry || display.busy() || (!entry.field_dirty && !entry.keys_dirty)) return;
        // The enter key follows the text length, so a text change can also repaint the keys.
        const bool field = entry.field_dirty, keys = entry.keys_dirty || entry.ready_drawn != entry_ready();
        draw_entry();
        display.present({{"ui", 0, 0, ++ui_sequence}, {}, FramePurpose::Overlay, RefreshIntent::Content,
                         field && keys ? RefreshRegion{0, 104, 684, 1112} : field ? entry_field_region : entry_keys_region});
    }
    void entry_done();
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
    // Step at which the last check of this start stopped; None after a check that read a manifest.
    enum class CheckStage : std::uint8_t { None, Wifi, Internet, Server };
    struct CheckResult {
        CheckStage stage = CheckStage::None;
        std::string ssid, note, body;
    };
    CheckResult update_check;
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
    /** Why the battery forbids installation; empty at 20% or above, or while charging. */
    std::string update_battery_block() const {
        if (battery_charging || (battery_percent && *battery_percent >= 20)) return {};
        return battery_percent ? "电量 " + std::to_string(*battery_percent) + "%。充电或电量到 20% 以上再安装。"
                               : "电量未知。请确认设备正在充电后再安装。";
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
    void layout_notes(const std::string& markdown, bool truncated, int first_top);
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
    enum class CellState : std::uint8_t { Done, Active, Failed, Waiting };
    /** One of the three check-status cells (Wi-Fi, internet, update server) at x; Waiting is dashed and gray. */
    void check_cell(int x, int y, const char* label, CellState state, const std::string& value, const std::string& note = "") {
        static constexpr int w = 196, h = 176;
        const std::uint8_t ink = state == CellState::Waiting ? 5 : 1;
        if (state != CellState::Waiting) outline(x, y, w, h, state == CellState::Failed ? 3 : 2);
        else {
            for (int i = 0; i < w; i += 12) {
                canvas.fill_rect(x + i, y, std::min(6, w - i), 2, ink);
                canvas.fill_rect(x + i, y + h - 2, std::min(6, w - i), 2, ink);
            }
            for (int i = 0; i < h; i += 12) {
                canvas.fill_rect(x, y + i, 2, std::min(6, h - i), ink);
                canvas.fill_rect(x + w - 2, y + i, 2, std::min(6, h - i), ink);
            }
        }
        text(label, x + 20, y + 20, 22, w - 36, ink);
        text(value, x + 20, y + 68, 28, w - 36, ink, true);
        if (!note.empty()) text(note, x + 20, y + 126, 22, w - 36, ink);
    }
    /** The three cells from y: steps before failed are done, the failed one is framed in 3 px, later ones did not run. */
    void check_cells(int y, CheckStage failed, const std::string& ssid, const std::string& note) {
        const bool wifi = failed == CheckStage::Wifi, internet = failed == CheckStage::Internet;
        check_cell(36, y, "Wi-Fi", wifi ? CellState::Failed : CellState::Done, wifi ? "未连接" : "已连接", wifi ? note : ssid);
        if (wifi) check_cell(244, y, "互联网", CellState::Waiting, "未进行");
        else check_cell(244, y, "互联网", internet ? CellState::Failed : CellState::Done, internet ? "无法访问" : "正常", internet ? note : "");
        if (failed != CheckStage::Server) check_cell(452, y, "更新服务器", CellState::Waiting, "未进行");
        else check_cell(452, y, "更新服务器", CellState::Failed, "异常", note);
    }
    /** Day and time of the cached check, such as "今天 10:24"; empty when the time is unknown. */
    std::string update_checked_at() const {
        const auto checked = update_checked();
        return checked.rfind("上次检查 ", 0) == 0 ? checked.substr(std::string("上次检查 ").size()) : "";
    }
    /** preview_bottom limits the release-note excerpt; a panel drawn over the page passes its own top. */
    void draw_update_home(update::Offer offer, int preview_bottom = 700);
    void draw_update();
    /** Queues a check or a download for the network worker, or shows why it cannot start. */
    void update_begin(UpdateOperation::Kind kind);
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
    void update_complete(std::uint64_t id, UpdateFailure failure, int status, const std::string& body, const std::string& detail,
                         LinkFailure link, const std::string& ssid);
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
        // Pages that offer an installation show a failed check only through the notice.
        if (update_check.stage != CheckStage::None) update_notice = update_check.body;
        if (!update_waiting()) {
            // A manifest fetched since this start stays; the card is read only for one that is not.
            if (!update_ready && !update_fresh) load_update_state();
            update_page = UpdatePage::Home;
            note_lines.clear(); note_page = 0;
        }
        draw_update(); overlay();
    }
    /** Handles a control of the update page; false leaves id to the common controls. */
    bool update_control(int id);
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
    void draw_settings();
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
            const bool hidden = help_page == 3 && ((zone.id == HelpPosition && help_position == 4) ||
                (zone.id == HelpDevice && help_device == 2));
            canvas.fill_rect(zone.x, zone.y, zone.w, zone.h, zone.id == help_zone && !hidden ? 1 : 15);
            hits.push_back(zone);
        }
        for (int split : {left, right}) canvas.line(x + split, y, x + split, y + foot, 9, 2);
        canvas.line(x, y + foot, x + width, y + foot, 9, 2);
        canvas.line(x + device, y + foot, x + device, y + height, 9, 2);
        outline(x, y, width, height);
    }
    void draw_help_demo();
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
    // Rotation image of the prepared sleep frame, and the presented one a later LockScreen job records.
    std::string custom_rotation_name, custom_rotation_shown;
    std::chrono::steady_clock::time_point custom_retry_at{};
    std::optional<std::chrono::steady_clock::time_point> custom_sleep_deadline;
    std::unique_ptr<std::uint8_t, decltype(&std::free)> sleep_wait_footer{nullptr, &std::free};
    bool sleep_wait_restore_pending = false;
    View sleep_wait_view = View::Library;
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
    void preview_lock_screen() {
#ifdef ESP_PLATFORM
        if (draft.sleep_overlay && (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 342U * 1216 + 768U * 1024 ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 342U * 1216)) {
            draw_power("预览内存不足，请稍后重试。"); overlay(); return;
        }
#endif
        abandon_content();
        if (counting_job) counting_job->cancel();
        if (cover_job) cover_job->cancel();
        ContentJob::Request request;
        request.kind = ContentJob::Kind::LockScreenPreview; request.root = root;
        request.sleep_image = draft.sleep_image; request.sleep_overlay = draft.sleep_overlay;
        request.generation = foreground_generation;
        if (draft.sleep_overlay) request.lock_background = std::make_shared<graphics::Canvas>(canvas);
        pending_content = std::move(request);
        loading("正在准备图片预览");
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
    void present_sleep_wait_footer() {
        display.present({{"ui", 0, 0, ++ui_sequence}, {}, FramePurpose::Overlay,
            RefreshIntent::Content, RefreshRegion{0, footer_top, 684, 1216 - footer_top}});
    }
    void restore_sleep_wait() {
        if (view != sleep_wait_view) {
            sleep_wait_footer.reset();
            sleep_wait_restore_pending = false;
            return;
        }
        if (!sleep_wait_footer || display.busy()) return;
        std::memcpy(canvas.data() + 342 * footer_top, sleep_wait_footer.get(), kSleepWaitFooterBytes);
        sleep_wait_footer.reset();
        present_sleep_wait_footer();
    }
    void show_sleep_wait() {
        if (display.busy() || sleep_wait_footer) return;
#ifdef ESP_PLATFORM
        if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < kSleepWaitFooterBytes + 64U * 1024) return;
#endif
        sleep_wait_footer.reset(static_cast<std::uint8_t*>(std::malloc(kSleepWaitFooterBytes)));
        if (!sleep_wait_footer) return;
        sleep_wait_view = view;
        std::memcpy(sleep_wait_footer.get(), canvas.data() + 342 * footer_top, kSleepWaitFooterBytes);
        canvas.fill_rect(0, footer_top, 684, 1216 - footer_top, 15);
        text("正在准备休眠，按电源键取消", 36, footer_top + 28, 24, 612, 1);
        present_sleep_wait_footer();
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
    void prepare_custom_sleep(bool sleeping);
    void draw_power(const std::string& preview_notice = {});
    static std::string server_name(const sync::Config &account) {
        const auto scheme = account.endpoint.find("://");
        return scheme == std::string::npos ? account.endpoint : account.endpoint.substr(scheme + 3);
    }
    static std::string typography_summary(const ReaderSettings &values) {
        return font_name(values.font) + " · 字号 " + std::to_string(values.font_size_px) + " · 行距 " + compact(values.line_height);
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
    std::optional<std::uint64_t> tree_bytes(const std::string &relative, bool books_only);
    void draw_storage();
    void draw_input_settings() {
        if (!bookmark_io_ready()) return;
        view = View::InputSettings;
        hits.clear(); canvas.clear(); header("书库", Back, "设置");
        const auto values = store.load_settings().settings;
        value_row(LibraryView, "书库默认视图", "点按切换", values.library_view == "grid" ? "卡片" : "列表", 104, 104, false, false);
        value_row(LibraryFolders, "书库文件夹", "最近阅读排序时生效", values.library_folders == "first" ? "置顶" : "跟随最近", 208, 104, false, false);
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
    void draw_sync_settings();
    void verify_account();
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
    void inspect_book(const std::string &path, bool keep_message = false);
    void draw_details();
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
    void manage_book();
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
    void draw_sleep(const std::string &sync_state);
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
    void draw_import();
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
    // Row of the rule above the upload summary on the page that shows the codes.
    int portal_summary_line = 0;
    void draw_portal();
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
    void draw_sync();
    /** Starts one check. automatic lets this check alone use any saved network instead of the pinned one. */
    void check_sync(bool automatic = false);
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
        loading_origin = origin_for(view);
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
            else if (detail == sync::kLongSourceNode)
                sync_status = "这一章含有超长文本节点，暂时无法准确换算位置，没有跳转。";
            else if (outside_mapping(detail))
                sync_status = "远端位置所在的章节含有表格等暂不支持的内容，无法换算成本机位置，没有跳转。";
        }
    }
    /** True when the content at a position, not a transient condition, keeps it from being mapped. */
    static bool outside_mapping(const std::string& detail) {
        return detail == sync::kUnsupportedStructure || detail == sync::kUnresolvedVisibility || detail == sync::kLongSourceNode;
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
            detail == sync::kLongSourceNode ? "这一章含有超长文本节点，暂时无法准确换算位置，没有上传。" :
            outside_mapping(detail) ? "这一章含有表格等暂不支持的内容，无法换算成 KOReader 的位置，没有上传。" :
            "这个位置暂时不能上传。";
    }
    void upload();
    void send_upload(const ReadingRecord& record, const std::string& xpointer, double percent);
    void network_complete(std::uint64_t id, int status, const std::string &bytes,
                          const std::string &failure, LinkFailure link);
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
    void error(const std::string &text_in, const std::string &retry = "");
    void back();
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
    void wifi_control(int id);
    void control(int id);
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
        case View::DictionarySettings:
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
    void input(const AppInput &input);
};


} // namespace pico_page
