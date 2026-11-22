#pragma once

#include "content_cache.h"
#include "pico_page/i18n.h"
#include "content_job.h"
#include "help_content.h"
#include "font_error_message.h"
#include "reading_footer.h"
#include "pico_page/stage_timer.h"
#include "pico_page/host_work_probe.h"
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
inline constexpr int fullscreen_margin_y_px = 24;
inline constexpr int fullscreen_notice_top = 1216 - fullscreen_margin_y_px + 2;
inline constexpr std::size_t kSleepWaitFooterBytes = 342U * (1216U - footer_top);
inline constexpr char kProjectHome[] = "picopage.wulu.zone";
inline constexpr int viewer_left = 24, viewer_top = 104, viewer_width = 636, viewer_height = 1088;
inline constexpr int viewer_toolbar_height = 96, viewer_action_left = 456;
inline constexpr int viewer_contrast_levels = 3;
inline constexpr auto inline_image_delay = std::chrono::milliseconds(250);
inline constexpr auto dictionary_loading_panel_delay = std::chrono::milliseconds(500);
inline constexpr std::size_t kMaxScannedFonts = 256;
inline constexpr std::size_t kMaxFontChoices = kMaxScannedFonts + 1;
inline constexpr const char* kSleepFallbackNotice = "本次使用简洁屏保";
inline constexpr const char *content_revision = "epub-native-v1";
struct FontSizeRestore {
    graphics::Font* font;
    int pixels;
    FontSizeRestore(graphics::Font& value, bool restore)
        : font(restore ? &value : nullptr), pixels(value.pixel_size()) {}
    ~FontSizeRestore() { if (font && pixels > 0 && font->pixel_size() != pixels) font->set_pixel_size(pixels); }
};

struct Entry {
    std::string path, name;
    bool directory;
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
    Apply,
    Scope,
    FontChoice,
    FontWeight,
    Gap,
    Indent,
    Defaults,
    FullscreenToggle,
    AlignmentToggle,
    OriginalWeightLess,
    OriginalWeightMore,
    EpubLayoutUnified,
    EpubLayoutOriginal,
    TxtKeepLineBreaks,
    TxtReflowParagraphs,
    FontSizeLess,
    FontSizeMore,
    FontWeightLess,
    FontWeightMore,
    LineHeightLess,
    LineHeightMore,
    MarginLess,
    MarginMore,
    GapLess,
    GapMore,
    IndentLess,
    IndentMore,
    RefreshClear,
    RefreshBalanced,
    RefreshFast,
    SaveRetry,
    LibraryRetry,
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
    PageEffects, EffectOff, EffectRipple, EffectDiagonal, EffectTouchPosition,
    IntervalLess,
    IntervalMore,
    IntervalSave,
    PowerSettings,
    StorageSettings,
    InputSettings,
    LanguageSettings, LanguageSimplified, LanguageTraditional, LanguageEnglish,
    SleepCover,
    SleepReading,
    SleepSimple,
    SleepLess,
    SleepMore,
    PowerSave,
    ClearCache,
    DetailOpen,
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
    Help, About, OnboardingReplay, OnboardingStart, OnboardingNext, OnboardingFinish,
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
    EntryBase = 128,
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
    WifiClock, WifiClockCancel, SyncClockEnable, SyncSubProgress, SyncSubClock,
    WifiSavedBase = 50100, WifiModeBase = 50200, WifiNearbyBase = 50300,
    SleepCustom = 51000, SleepRotation, SleepChoose, SleepRefresh, SleepOverlay, SleepPreview, LockScreenBase = 52000,
    UpdateOpen = 53000, UpdateCheck, UpdateNotes, UpdateStart, UpdateConfirm, UpdateCancel,
    UpdateRetry, UpdateLater, UpdateRestart, UpdateDismiss,
    FontEntryBase = 54000, FontAccept = 54300, FontCancel,
    DictionaryOpen = 55000, DictionaryScan, DictionaryClose, DictionaryExpand, DictionaryRetry,
    DictionarySources, DictionaryNextEntry, DictionaryStart = 55008, DictionaryEnd, DictionaryManage,
    DictionaryPreviousSource, DictionaryNextSource,
    DictionaryStartPrevious, DictionaryStartNext, DictionaryEndPrevious, DictionaryEndNext,
    DictionaryToggleBase = 56000, DictionaryUpBase = 56100, DictionaryDownBase = 56200, DictionarySourceBase = 56400
};
static_assert(ContentsBookmarks < EntryBase);
struct PageEffectChoice {
    PageEffect effect;
    const char* title;
    const char* description;
};
inline constexpr PageEffectChoice page_effect_choices[] = {
    {PageEffect::Off, "关闭", "沿用当前刷新模式"},
    {PageEffect::Ripple, "水波纹", "条带依次展开，翻页可能更慢"},
    {PageEffect::Diagonal, "斜掠", "从页角斜向展开，翻页可能更慢"},
    {PageEffect::TouchPosition, "随点按位置", "上下斜掠，中部和按键水平展开"},
};
static_assert(std::size(page_effect_choices) == EffectTouchPosition - EffectOff + 1);
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
    return txt_name(path) ? txt_content_revision(metadata.txt_encoding) : content_revision;
}
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
}
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
        const bool opened = reuse ? document.open_verified(full, found->second.identity, error, {},
            path == book_path ? verified_reader_index() : EpubDocument::VerifiedIndex{}) :
            document.open(full, error, {}, path == book_path ? document.book_index() : EpubDocument::VerifiedIndex{});
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
    graphics::Font library_font{128U * 1024, 1024};
    std::string library_font_path;
    bool library_uses_body = false, library_font_reload = true;
    Display display{*this};
    Session session{display};
    InputRouter router;
    EpubDocument document;
    EpubDocument::VerifiedIndex verified_reader_index() const {
        const auto found = identities.find(book_path);
        if (!controlled_storage || found == identities.end() ||
            found->second.identity.book_id != document.metadata().book_id) return {};
        return document.verified_epub_index();
    }
    Chapter chapter;
    graphics::AuthorFonts author_fonts, counting_fonts;
    bool author_fonts_stable = true, author_fonts_released = false;
    void release_author_fonts() {
        author_fonts_released |= !author_fonts.faces.empty();
        author_fonts.clear();
        typography_preview.reset();
    }
    graphics::Page page;
    std::optional<graphics::Canvas> body_canvas;
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
               a.indent_chars == b.indent_chars && a.paragraph_gap == b.paragraph_gap && a.justify == b.justify &&
               a.original_weight_adjustment == b.original_weight_adjustment &&
               a.original_epub == b.original_epub && a.txt_reflow == b.txt_reflow &&
               a.bleed_to_screen_top == b.bleed_to_screen_top;
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
    struct TypographyPreview {
        Chapter chapter;
        graphics::AuthorFonts fonts;
        bool fonts_stable = true;
        ContentCacheEntry cache;
    };
    std::optional<TypographyPreview> typography_preview;
    std::size_t typography_preview_offset = 0;
    bool typography_preview_failed = false;
    enum class OnboardingStep { Language, Welcome, Practice, Tools };
    enum class OnboardingTask { Next, Previous, OpenTools, CloseTools, Done };
    OnboardingStep onboarding_step = OnboardingStep::Language;
    OnboardingTask onboarding_task = OnboardingTask::Next;
    int onboarding_page = 0, onboarding_tool = 0;
    bool onboarding_from_help = false, onboarding_tools_open = false;
    bool sleep_onboarding = false, error_onboarding_help = false;
    void draw_onboarding();
    void draw_practice(const std::string& finish);
    void start_onboarding(bool from_help);
    void start_practice();
    void finish_onboarding();
    void onboarding_input(const AppInput& input);
    void draw_turn_regions(int x, int y, int width, int height, int selected, std::uint8_t divider = 1);
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
    bool power_progress_saved = false, power_reading = false;
    void draw_power_menu() {
        view = View::PowerMenu;
        hits.clear(); canvas.clear(); header(tr("电源"), 0);
        if (!power_save_failed) {
            text(tr("关机或重启"), 36, 184, 32, 612, 1, true);
            wrapped(tr("菜单出现后松开电源键。选择后保存本地阅读位置，不等待云端同步。"), 36, 276, 612, 24);
            button(PowerShutdown, tr("关机"), 24, 480, 636, 96);
            button(PowerRestart, tr("重启"), 24, 612, 636, 96);
            button(PowerCancel, tr("取消"), 24, 1080, 636, 96);
        } else {
            const bool restart = power_selected == PowerAction::Restart;
            text(tr("本地保存未完成"), 36, 184, 32, 612, 1, true);
            wrapped(message, 36, 276, 612, 24);
            wrapped(tr("继续可能丢失最后一段阅读进度。"), 36, 456, 612, 24);
            button(PowerCancel, tr("返回"), 24, 1080, 306, 96, true);
            danger_button(PowerForce, restart ? tr("仍然重启") : tr("仍然关机"), 354, 1080, 306, 96);
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
    std::string power_saved_note() const {
        if (!power_reading) return "";
        return power_progress_saved ? tr("阅读位置已保存。") : tr("阅读位置没有完整保存。");
    }
    void draw_power_off(const std::string& detail, bool manual = false) {
        view = View::PowerOff;
        message = detail;
        hits.clear(); canvas.clear(); header(tr("电源"), 0);
        text(manual ? tr("请手动关机") : power_selected == PowerAction::Restart ? tr("正在重启") : tr("已关机"),
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
    network::Hold manual_hold, sync_hold, wifi_hold, account_hold;
    std::optional<WifiTargets> manual_target, sleep_wifi_target;
    std::optional<WifiOperation> wifi_request;
    std::uint64_t wifi_sequence = 0, wifi_worker = 0;
    WifiOperation::Kind wifi_kind = WifiOperation::Kind::Connect;
    std::vector<provision::SavedWifi> wifi_saved;
    std::vector<provision::NearbyWifi> wifi_nearby;
    std::string pinned_wifi_ssid, wifi_notice, wifi_choice, pending_wifi_record;
    std::int64_t last_clock_sync_at = 0, pending_clock_sync_at = 0;
    enum class ClockFeedback { Idle, Success, Cancelled, Failed };
    ClockFeedback clock_feedback = ClockFeedback::Idle;
    std::string clock_notice;
    bool clock_from_sync = false;
    enum class WifiPage { Home, Mode, Saved, Detail, Forget, Add, Joining, JoinFailed, Joined };
    WifiPage wifi_page = WifiPage::Home, wifi_entry_page = WifiPage::Home;
    bool wifi_from_sync = false, wifi_from_transfer = false, wifi_scanned = false, account_from_sync = false;
    std::size_t wifi_add_page = 0;
    provision::WifiCredentials join_draft;
    bool join_manual = false, join_open = false, join_secured = false;
    bool join_link = false;
    WifiPage join_origin = WifiPage::Add;
    LinkFailure join_failure = LinkFailure::None;
    std::string join_error;
    bool join_unsaved = false;
    struct TextEntry {
        enum class Field { None, WifiPassword, WifiName, Endpoint, Username, Password };
        Field field = Field::None;
        std::string value;
        bool masked = false, shift = false;
        enum class Layer { Letters, Digits, Symbols };
        Layer layer = Layer::Letters;
        bool field_dirty = false, keys_dirty = false;
        bool enter_key_drawn_enabled = true;
    } entry;
    struct AccountDraft {
        std::string endpoint, username, password;
        bool had_saved_config = false;
        bool changed = false;
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
    bool portal_station = false;
    bool portal_ready_received = false;
    std::optional<Frame> pending;
    std::optional<NetworkOperation> network_request;
    std::shared_ptr<std::atomic_bool> network_cancel;
    std::optional<Anchor> remote_anchor;
    std::optional<sync::PartPosition> remote_place;
    std::optional<ReadingRecord> upload_record, acknowledged_record, uncertain_record;
    sync::Config acknowledged_config, uncertain_config;
    enum class SyncState { Unchecked, NoRecord, Same, Different, Unmapped, Uploaded, Uncertain,
                           Unauthorized, Failed, LinkFailed, ConfigChanged };
    SyncState sync_state = SyncState::Unchecked;
    enum class SyncSubtab { Progress, Clock };
    SyncSubtab sync_subtab = SyncSubtab::Progress;
    double remote_fraction = -1;
    std::string remote_device;
    bool sync_link_pinned = false;
    std::string sync_link_name;
    sync::Config sync_config, badge_config;
    bool badge_configured = false, tools_save_pending = false;
    void refresh_badge_config() {
        std::string detail;
        badge_configured = sync::load_config(root, badge_config, detail);
    }
    std::uint64_t network_sequence = 0, active_network = 0;
    std::uint64_t network_worker_id = 0;
    int network_after_cancel = 0;
    bool network_closing_upload = false;
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
    static constexpr std::size_t kCoverSlots = 8;
    std::vector<CoverThumbnail> covers;
    std::unique_ptr<ContentJob> cover_job;
    std::optional<ContentJob::Request> pending_covers;
    std::vector<ContentJob::CoverSource> missing_covers;
    std::uint64_t cover_generation = 0;
    static constexpr unsigned kCoverAttempts = 3;
    unsigned cover_attempts = 0;
    std::string cover_notice;
    std::chrono::steady_clock::time_point cover_retry_after{};
    void cancel_covers() {
        ++cover_generation; pending_covers.reset(); missing_covers.clear();
        if (cover_job) cover_job->cancel();
    }
    void stop_covers() { cancel_covers(); cover_job.reset(); }
    void poll_covers();
    std::vector<Hit> hits, links;
    std::optional<Anchor> link_return, confirmed_link_return;
    sync::PartPosition confirmed_page_end;
    bool sync_return = false, confirmed_sync_return = false;
    struct PageBoundary {
        std::size_t chapter, start, end;
        std::uint32_t part;
        bool return_bar, part_end, last_part;
    };
    std::vector<PageBoundary> page_history, computed_pages;
    void clear_page_history() { page_history.clear(); computed_pages.clear(); }
    void record_boundary(std::vector<PageBoundary>& entries, std::size_t start, std::size_t end) {
        const auto next = readable_offset(end);
        for (const auto& entry : entries)
            if (entry.chapter == chapter_index && entry.part == chapter.part &&
                entry.return_bar == link_return.has_value() && entry.start == start && entry.end == next) return;
        if (entries.size() == 64) entries.erase(entries.begin());
        entries.push_back({chapter_index, start, next, chapter.part, link_return.has_value(),
                           next == chapter.text.size(), chapter.last_part});
    }
    std::size_t chapter_index = 0, offset = 0, library_page = 0, toc_page = 0;
    std::vector<std::size_t> toc_starts, toc_visible;
    std::set<std::size_t> toc_collapsed;
    bool toc_locate_current = false, library_grid = false;
    std::string library_sort = "recent";
    bool library_resume_omitted = false, folders_first = true, library_recent = true;
    std::int64_t resume_opened_at = 0;
    BookmarkList bookmarks;
    bool contents_bookmarks = false;
    std::size_t bookmark_page = 0;
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
            return tr("书签文件与当前原书不符，未使用其中的书签。原文件已保留；本书加书签时，旧文件改名后保留在同一目录。");
        case StoreStatus::Invalid:
            return tr("书签文件无法读取，已保留原文件，暂不能添加或删除书签。阅读进度不受影响。");
        case StoreStatus::UnsupportedVersion:
            return tr("书签文件由较新的固件写入，本固件无法读取，原文件未修改。阅读进度不受影响。");
        default:
            return tr("书签文件读取失败，请检查 TF 卡后重新打开本书。阅读进度不受影响。");
        }
    }
    std::unique_ptr<ContentJob> bookmark_job;
    bool bookmark_adding = false;
    void toggle_bookmark();
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
    bool bookmark_drain_failed = false;
    std::string bookmark_drain_message;
    std::string bookmark_error;
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
    bool bookmark_io_ready() {
        drain_bookmark();
        if (!bookmark_drain_failed) return true;
        bookmark_drain_failed = false;
        bookmark_error.clear();
        error(bookmark_drain_message);
        return false;
    }
    bool finish_bookmark(bool redraw);
    std::string library_home_root;
    bool folder_root = false;
    enum class LibraryScan { Complete, Limited, Partial, Unavailable, Missing, NotDirectory, CreateFailed };
    LibraryScan library_scan = LibraryScan::Complete;
    bool library_create_full = false, library_create_denied = false;
    bool library_scan_failed() const {
        return library_scan != LibraryScan::Complete && library_scan != LibraryScan::Limited &&
               library_scan != LibraryScan::Partial;
    }
    const char* library_scan_notice() const;
    struct LibraryProblem {
        std::string title, summary;
        std::vector<std::string> steps;
        int action = 0;
        const char* action_label = "";
    };
    LibraryProblem library_problem() const;
    void draw_library_problem(int top);
    CardIssue card_issue = CardIssue::Removed;
    reading_footer::Position footer_position = reading_footer::Position::Pages;
    reading_footer::Device footer_device = reading_footer::Device::Battery;
    bool footer_charging = false;
    PageEffect page_effect = PageEffect::Off;
    bool refresh_from_settings = false, import_from_settings = false, help_from_settings = false, typography_from_settings = false;
    Origin origin_for(View source) const;
    const char *return_label(Origin origin) const {
        if (view == View::Error && error_onboarding_help) return tr("返回使用说明");
        switch (origin) {
        case Origin::Reading: return tr("恢复阅读");
        case Origin::Library: return tr("返回书库");
        case Origin::Settings: return tr("返回设置");
        }
        return tr("返回书库");
    }
    void return_to_origin(Origin origin, bool full = false) {
        if (view == View::Error && error_onboarding_help) {
            error_onboarding_help = false; help_page = 0; draw_help(); overlay(full); return;
        }
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
    bool turned = false;
    static constexpr const char *kFallbackFontNotice = "所选字体不可用，已使用替代字体";
    static constexpr const char *kWeightUnavailableNotice = "所选字重不可用，保留字体原有字重";
    static constexpr const char *kBuiltinFontNotice = "未找到正文字体，已使用内置字体";
    static constexpr const char *kMissingNotice = "部分字符无法显示，可在排版中更换字体";
    static constexpr const char *kSaveFailed = "进度未保存，请检查 TF 卡后重试";
    static constexpr std::size_t not_counting = static_cast<std::size_t>(-1);
    static constexpr std::size_t max_pages_bytes = kPageIndexBytes;
    PageIndices chapter_pages;
    std::vector<bool> counted;
    std::vector<bool> counted_live;
    std::string pages_name, pages_header, font_identity;
    std::shared_ptr<FileDigest> font_scan;
    bool font_scan_turn = true, pages_cache_disabled = false;
    std::string body_font_path() const {
        return builtin_body ? "" : root + "/" +
            (font_fallback ? "Fonts/LXGWWenKai-Regular.ttf" : effective_font);
    }
    Chapter counting_chapter;
    bool async_content = false;
    std::uint64_t counting_generation = 0;
    std::shared_ptr<PaginationSession> counting_session;
    void invalidate_counting() { ++counting_generation; counting_session.reset(); }
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
    std::chrono::steady_clock::time_point dictionary_panel_due{}, dictionary_slow_due{};
    bool dictionary_slow = false;
    std::vector<DictionaryItem> dictionary_items;
    std::vector<DictionaryItem> dictionary_failed_sources;
    std::size_t dictionary_list_page = 0, dictionary_source = 0, dictionary_entry = 0, dictionary_entry_base = 0;
    bool dictionary_list_complete = true, dictionary_full = false, dictionary_search_all = true;
    bool dictionary_missing_glyphs = false, dictionary_omitted_fields = false;
    bool dictionary_previous_exhausted = false, dictionary_next_exhausted = false;
    int dictionary_retry_direction = 0;
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
    void navigate_dictionary(int direction);
    void draw_dictionary_settings();
    void begin_dictionary(int x, int y, bool selecting = false);
    bool dictionary_contact_input(const AppInput& input, bool accepted);
    void submit_dictionary_selection();
    void flush_dictionary_selection();
    void reset_dictionary_contact();
    void query_dictionary(bool retry = false, bool defer_panel = false);
    void poll_dictionary_panel();
    void page_dictionary(int movement = 0, bool redraw = true);
    void draw_dictionary();
    void finish_dictionary(ContentJob& job);
    void dictionary_control(int id, bool by_scalar = false);
    void close_dictionary();
    std::optional<dictionary::Selection> dictionary_range_step(int id, bool by_scalar) const;
    std::pair<int, int> dictionary_bounds(bool full) const;
    graphics::LayoutSpec dictionary_layout(bool full) const;
    std::shared_ptr<const ContentCacheEntry> chapter_cache, recent_chapter_cache;
    bool chapter_cache_write_needed = false, recent_cache_write_needed = false;
    void adopt_chapter_cache(ContentCacheEntry entry) {
        if (entry.bytes.empty()) return;
        chapter_cache_write_needed = !entry.from_disk;
        chapter_cache = std::make_shared<ContentCacheEntry>(std::move(entry));
    }
    std::unique_ptr<ContentJob> prepared_chapter;
    std::shared_ptr<ChapterSession> chapter_session;
    std::size_t chapter_stream_bytes = 0;
    void release_chapter_session() {
        if (chapter_session) {
            if (counting_job && counting_job->request.chapter_session == chapter_session) counting_job->cancel();
            if (prepared_chapter && prepared_chapter->request.chapter_session == chapter_session) prepared_chapter.reset();
        }
        chapter_session.reset(); chapter_stream_bytes = 0;
    }
    std::uint64_t chapter_prefetch_hits = 0;
    std::size_t prepared_chapter_size_bytes = 0;
    bool chapter_prefetch_attempted = false;
    bool chapter_prefetch_due() const;
    void prepare_chapter();
    void finish_chapter(ContentJob& job, PageTurnOrigin origin);
    std::optional<std::chrono::steady_clock::time_point> chapter_feedback_after;
    std::unique_ptr<graphics::Canvas> prepared_image;
    std::vector<std::size_t> prepared_image_failures;
    std::uint64_t foreground_generation = 0;
    PP_DIAGNOSTIC(std::uint64_t diagnostic_session = 0, diagnostic_layout = 0;)
    bool content_loading = false, image_prepared = false;
    bool inline_image_pending = false, body_images_pending = false;
    std::chrono::steady_clock::time_point inline_image_after = std::chrono::steady_clock::time_point::max();
    std::string prepared_image_error;
    enum class ChapterCacheRetention { Discard, Keep };
    void abandon_content(ChapterCacheRetention cache_retention = ChapterCacheRetention::Discard) {
        cancel_covers();
        ++foreground_generation;
        if (foreground_job) foreground_job->cancel();
        pending_content.reset();
        if (cache_retention == ChapterCacheRetention::Discard) {
            chapter_cache.reset(); recent_chapter_cache.reset();
            chapter_cache_write_needed = recent_cache_write_needed = false;
        }
        prepared_chapter.reset(); chapter_prefetch_attempted = false;
        release_chapter_session();
        chapter_feedback_after.reset();
        content_loading = image_prepared = false;
        inline_image_pending = false;
        prepared_image.reset();
        prepared_image_failures.clear();
    }
    Hit loading_cancel{};
    void loading(const std::string &status) {
        loading_origin = origin_for(view);
        const bool panel = view == View::Tools || view == View::Contents || view == View::Typography ||
                           view == View::Refresh || view == View::Sync || view == View::Confirmation;
        if (panel && body_canvas) canvas = *body_canvas;
        else if (panel || view == View::Error) canvas.fill_rect(0, 104, 684, 1112, 15);
        const int top = view == View::StorageSettings ? 1080 :
            panel || view == View::Reading || view == View::Error ? footer_top : footer_zone;
        content_loading = true;
        view = View::Loading;
        hits.clear();
        canvas.fill_rect(0, top, 684, 1216 - top, 15);
        if (top == footer_zone) canvas.line(0, top, 683, top, 1, 2);
        text(status, 36, 1156, 24, 400, 3);
        button(Dismiss, tr("取消"), 488, 1140, 160, 64, false, 24);
        loading_cancel = hits.back();
        overlay();
    }
    void close_image_viewer() {
        viewer_original.reset();
        image_viewing = false; image_contrast_level = 0;
        view = View::Reading;
        if (body_canvas && body_bookmarked != bookmarked(page.start, page.end) && render_body(offset)) { overlay(); return; }
        if (body_canvas) canvas = *body_canvas;
        links = body_links;
        overlay();
    }
    void draw_image_viewer(bool full = false);
    const graphics::ImageBox* image_at(int x, int y) const {
        if (view != View::Reading || image_viewing || content_loading || display.busy() ||
            pending || !card_available || usb_prepared || y >= reading_touch_bottom())
            return nullptr;
        for (const auto& link : links)
            if (link.contains(x, y)) return nullptr;
        for (const auto& box : page.images)
            if (!box.resource.empty() && x >= box.x && x < box.x + box.width &&
                y >= box.y && y < box.y + box.height)
                return &box;
        return nullptr;
    }
    void trim_glyph_caches_for_free_memory(std::size_t required_free_bytes) {
#ifdef ESP_PLATFORM
        for (auto* font : {&library_font, &body, &ui}) {
            const auto available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            if (available >= required_free_bytes) return;
            const auto cached = font->cached_glyph_bytes();
            font->trim_glyph_cache(cached - std::min(cached, required_free_bytes - available), font->cached_glyphs());
            if (font->cached_glyph_bytes() < cached) ++cache_evictions;
        }
#else
        static_cast<void>(required_free_bytes);
#endif
    }
    void make_room_for_cold_image() {
        trim_glyph_caches_for_free_memory(ContentJob::cold_image_memory_required_bytes + 256U * 1024);
    }
    void begin_image_viewer(const graphics::ImageBox& box) {
        if (!body_canvas && !body_cache_room()) {
            message = tr("内存不足，无法查看图片");
            footer(); overlay();
            return;
        }
        body_canvas = canvas;
        body_spec = reading_layout(settings, link_return.has_value());
        body_links = links;
        body_bookmarked = bookmarked(page.start, page.end);
        abandon_content();
        release_counting(); clear_prefetch(); make_room_for_cold_image();
        ContentJob::Request request;
        request.kind = ContentJob::Kind::ImageViewer;
        request.root = root; request.path = root + "/" + book_path; request.identity = document.metadata();
        request.page.images.push_back({box.begin, viewer_left, viewer_top, viewer_width, viewer_height, box.resource});
        request.image_resource = box.resource;
        request.generation = foreground_generation;
        request.chapter = chapter_index; request.part = chapter.part;
        pending_content = std::move(request);
        loading(tr("正在查看图片…"));
    }
    bool begin_image(std::size_t start, const graphics::Page& planned, bool preview = false, bool progressive = false);
    bool begin_open(const std::string& path, ContentJob::Kind kind = ContentJob::Kind::Open) {
        cancel_sync();
        ContentJob::Request request;
        request.kind = kind;
        if (kind == ContentJob::Kind::Open) {
            chapter_session = std::make_shared<ChapterSession>();
            request.chapter_session = chapter_session;
        }
        request.root = root; request.relative_path = path; request.path = root + "/" + path;
        request.generation = foreground_generation;
        struct stat info{};
        const auto found = identities.find(path);
        if (controlled_storage && found != identities.end() && stat(request.path.c_str(), &info) == 0 &&
            found->second.size == info.st_size && found->second.modified == info.st_mtime && found->second.inode == info.st_ino) {
            request.identity = found->second.identity;
            if (kind == ContentJob::Kind::Open && path == book_path) request.book_index = verified_reader_index();
        } else if (kind == ContentJob::Kind::Open && path == book_path) {
            request.book_index = document.book_index();
        }
        request.content_revision = revision_for(path, request.identity);
        if (kind == ContentJob::Kind::Inspect && stat(request.path.c_str(), &info) == 0) {
            const auto cover = cover_entry(path, info, 120, 160, false);
            request.cache_name = cover.name; request.cache_header = cover.header;
            request.layout.width = 120; request.layout.height = 160;
        }
        pending_content = std::move(request);
        clear_prefetch(); covers.clear();
        loading(tr("正在准备书籍…"));
        if (kind == ContentJob::Kind::Open) { clear_body_cache(); release_author_fonts(); release(chapter); page = {}; }
        return true;
    }
    bool begin_chapter(std::size_t index, std::uint32_t part, ContentJob::Target target,
                       std::size_t position = 0, PageTurnOrigin origin = PageTurnOrigin::Middle,
                       const Anchor& anchor = {}, const std::string& fragment = "");
    bool begin_seek(const Anchor& anchor) {
        std::size_t index;
        if (!document.chapter_index_for(anchor.resource, index)) { error(tr("目标位置不在正文中")); return false; }
        return begin_chapter(index, 0, ContentJob::Target::Anchor, 0, PageTurnOrigin::Middle, anchor);
    }
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
    std::optional<std::chrono::steady_clock::time_point> sleep_cover_deadline;
    bool sleep_waiting_for_cover = false;
    std::unique_ptr<graphics::Canvas> sleep_cover_image;
    bool sleep_screen_shows_book() const {
        return has_book() && (sleep_origin == Origin::Reading || settings.sleep_screen != "cover");
    }
    void prepare_sleep_cover() {
        if (counting_job) return;
        sleep_cover_image.reset();
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
    bool persist() {
        drain_save();
        save_error_pending = false;
        const bool bookmark_stored = !std::exchange(bookmark_drain_failed, false);
        if (!bookmark_stored) message = bookmark_drain_message;
        auto request = session.request_save();
        if (!request)
            return !session.needs_save() && bookmark_stored;
        const StageTimer timer{"progress_commit", 0};
        const auto result = store.save_progress(book_path, request->record);
        const bool okay =
            result.status == StoreStatus::Ok && result.record && *result.record == request->record;
        session.saved(*request, okay);
        save_failed = !okay;
        if (!okay)
            message = tr(kSaveFailed);
        return okay && bookmark_stored;
    }
    const char* tr(const char* source) const { return i18n::text(settings.language, source); }
    std::string trf(const char* source, std::initializer_list<i18n::Argument> arguments) const {
        return i18n::format(settings.language, source, arguments);
    }
    void text(const std::string &value, int x, int top, int size = 28, int max_width = 612,
              std::uint8_t gray = 1, bool bold = false);
    void text(graphics::Font& font, const std::string& value, int x, int top, int size = 28,
              int max_width = 612, std::uint8_t gray = 1, bool bold = false);
    static bool word_scalar(char32_t c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               c == '.' || c == '_' || c == '-' || c == '/' || c == '%';
    }
    static int run_width(graphics::Font& font, const std::u32string &scalars, std::size_t index) {
        if (!word_scalar(scalars[index]) || (index && word_scalar(scalars[index - 1]))) return 0;
        int width = 0;
        for (; index < scalars.size() && word_scalar(scalars[index]); ++index) width += font.advance(scalars[index]);
        return width;
    }
    int wrapped(const std::string &value, int x, int top, int width, int size = 26,
                bool draw = true, std::uint8_t gray = 1) {
        return wrapped(ui, value, x, top, width, size, draw, gray);
    }
    int wrapped(graphics::Font& font, const std::string &value, int x, int top, int width, int size = 26,
                bool draw = true, std::uint8_t gray = 1) {
        const FontSizeRestore restore(font, &font == &body);
        if (!font.set_pixel_size(size)) return 0;
        const auto metrics = font.metrics();
        const int line = metrics.ascender + metrics.descender + 2;
        const auto scalars = decode_utf8(value);
        int used = 0, rows = 1;
        for (std::size_t i = 0; i < scalars.size(); ++i) {
            const int advance = font.advance(scalars[i]), run = run_width(font, scalars, i);
            const bool closing = std::u32string_view(U"，。、；：！？）」』”’》").find(scalars[i]) != std::u32string_view::npos;
            if (used && !closing && (used + advance > width || (run <= width && used + run > width))) {
                used = 0;
                ++rows;
            }
            if (!used && rows > 1 && scalars[i] == U' ') continue;
            if (draw)
                font.draw(canvas, scalars[i], x + used, top + (rows - 1) * line + metrics.ascender, gray);
            used += advance;
        }
        return rows * line;
    }
    int text_width(const std::string &value, int size) { return text_width(ui, value, size); }
    int text_width(graphics::Font& font, const std::string &value, int size) {
        const FontSizeRestore restore(font, &font == &body);
        if (!font.set_pixel_size(size)) return 0;
        int length = 0;
        for (auto c : decode_utf8(value))
            length += font.advance(c);
        return length;
    }
    int top_for_middle(graphics::Font& font, int size, int middle, bool bold = false) {
        const FontSizeRestore restore(font, &font == &body);
        if (!font.set_pixel_size(size)) return middle - size / 2;
        int above = 0, rows = 0;
        if (!font.ink_rows(font.contains(U'中') ? U'中' : U'H', above, rows, bold) || !rows) return middle - size / 2;
        return middle + above - rows / 2 - font.metrics().ascender;
    }
    void center(const std::string &value, int x, int top, int w, int size = 28,
                std::uint8_t gray = 1, bool bold = false) {
        text(value, x + std::max(0, (w - text_width(value, size)) / 2), top, size, w, gray, bold);
    }
    void value_row(int id, const std::string &title, const std::string &description,
                   const std::string &value, int y, int height = 104, bool bold_value = false, bool opens = true) {
        const auto shown = !id || !opens ? value : value.empty() ? "›" : value + " ›";
        const int value_width = std::min(text_width(shown, 24), 400);
        const int title_width = 612 - value_width - 16;
        if (description.empty()) {
            if (text_width(title, 28) > title_width && height >= 88) {
                const int text_height = title_lines(title, 36, y, title_width, 28, false, 2, 1, false);
                title_lines(title, 36, y + (height - text_height) / 2, title_width, 28);
            } else text(title, 36, top_for_middle(ui, 28, y + height / 2), 28, title_width);
        } else {
            text(title, 36, y + height / 2 - 42, 28, title_width);
            text(description, 36, y + height / 2 + 4, 24, title_width, 3);
        }
        text(shown, 648 - value_width, top_for_middle(ui, 24, y + height / 2), 24, value_width, 1, bold_value);
        canvas.line(36, y + height - 1, 647, y + height - 1, 12, 2);
        if (id) hits.push_back({24, y, 636, height, id});
    }
    void outline(int x, int y, int w, int h, int weight = 2, std::uint8_t gray = 1) {
        canvas.fill_rect(x, y, w, weight, gray);
        canvas.fill_rect(x, y + h - weight, w, weight, gray);
        canvas.fill_rect(x, y, weight, h, gray);
        canvas.fill_rect(x + w - weight, y, weight, h, gray);
    }
    void button_label(const std::string& title, int x, int y, int width, int height, int size,
                      std::uint8_t gray, bool bold = false) {
        if (text_width(title, size) > width - 24) {
            const int text_height = title_lines(title, x + 12, y, width - 24, size, bold, 2, gray, false);
            if (text_height <= height - 12) {
                title_lines(title, x + 12, y + (height - text_height) / 2, width - 24, size, bold, 2, gray, true, true);
                return;
            }
            const int line_pitch = size + 8;
            if (height >= size + line_pitch + 8) {
                const int first_middle = y + (height - line_pitch) / 2;
                title_lines(title, x + 12, top_for_middle(ui, size, first_middle, bold), width - 24, size, bold, 2,
                            gray, true, true);
                return;
            }
        }
        center(title, x, top_for_middle(ui, size, y + height / 2), width, size, gray, bold);
    }
    void button(int id, const std::string &title, int x, int y, int w, int h = 88,
                bool selected = false, int size = 28, bool bold = false) {
        if (selected)
            canvas.fill_rect(x, y, w, h, 1);
        else
            outline(x, y, w, h);
        button_label(title, x, y, w, h, size, selected ? 15 : 1, bold);
        hits.push_back({x, y, w, h, id});
    }
    void header(const std::string &title, int back = Back, const std::string &parent = {}, int right_edge = 660,
                graphics::Font* title_font = nullptr, graphics::Font* parent_font = nullptr) {
        canvas.fill_rect(0, 0, 684, 104, 15);
        int left = 28;
        if (back) {
            const auto label = "‹ " + (parent.empty() ? std::string(tr("书库")) : parent);
            auto& font = parent_font ? *parent_font : ui;
            const int width = std::min(text_width(font, label, 28), 240);
            text(font, label, 28, 32, 28, width);
            left = 28 + width + 28;
            hits.push_back({0, 0, left, 104, back});
        }
        text(title_font ? *title_font : ui, title, left, 30, 32, right_edge - left, 1, true);
        canvas.line(0, 103, 683, 103, 1, 3);
    }
    void disabled_button(const std::string &label, int x, int y, int w, int h = 88, int size = 28) {
        button_label(label, x, y, w, h, size, 5);
    }
    void danger_button(int id, const std::string &label, int x, int y, int w, int h = 96) {
        outline(x, y, w, h);
        button_label(label, x, y, w, h, 28, 1);
        hits.push_back({x, y, w, h, id});
    }
    const std::string &current_notice() const {
        return view == View::Library && !cover_notice.empty() ? cover_notice : message;
    }
    const std::string &notice() {
        notice_shown = notice_shown || !current_notice().empty();
        return current_notice();
    }
    static constexpr int footer_zone = 1120;
    void footer_bar(const std::string &hint, std::size_t index = 0, std::size_t total = 1) {
        constexpr int width = 684, height = 96, side_width = 205, next_left = width - side_width, size = 24;
        canvas.fill_rect(0, footer_zone, width, height, 15);
        if (total <= 1 && hint.empty()) return;
        canvas.line(0, footer_zone, width - 1, footer_zone, 1, 2);
        if (total <= 1) {
            if (text_width(hint, size) <= 612) text(hint, 36, footer_zone + 34, size, 612);
            else {
                const int text_height = title_lines(hint, 36, 0, 612, size, false, 2, 1, false);
                title_lines(hint, 36, footer_zone + (height - text_height) / 2, 612, size);
            }
            return;
        }
        const bool library = view == View::Library;
        const int text_top = library ? top_for_middle(ui, size, footer_zone + height / 2) : footer_zone + 34;
        for (int x : {side_width, next_left}) canvas.line(x, footer_zone, x, footer_zone + height - 1, 1, 2);
        if (index) {
            if (library) center(tr("‹ 上一屏"), 0, text_top, side_width, size);
            else text(tr("‹ 上一屏"), 36, text_top, size, 160);
            hits.push_back({0, footer_zone, side_width, height, Prev});
        }
        if (index + 1 < total) {
            if (library) center(tr("下一屏 ›"), next_left, text_top, side_width, size);
            else text(tr("下一屏 ›"), 648 - text_width(tr("下一屏 ›"), size), text_top, size, 160);
            hits.push_back({next_left, footer_zone, side_width, height, Next});
        }
        if (hint.empty()) center(std::to_string(index + 1) + " / " + std::to_string(total), side_width, text_top, next_left - side_width, size, 3);
        else center(hint, side_width + 8, text_top, next_left - side_width - 16, size);
    }
    int title_lines(const std::string &value, int x, int y, int width, int size, bool bold = false, int max_rows = 2,
                    std::uint8_t gray = 1, bool draw = true, bool centered = false);
    int title_lines(graphics::Font& font, const std::string &value, int x, int y, int width, int size,
                    bool bold = false, int max_rows = 2, std::uint8_t gray = 1, bool draw = true, bool centered = false);
    bool typography_changed() const {
        const auto &applied = edit_snapshot.config.settings;
        return reset_overrides || draft.original_epub != applied.original_epub || draft.txt_reflow != applied.txt_reflow ||
            draft.original_weight_adjustment != applied.original_weight_adjustment || draft.justify != applied.justify ||
            draft.fullscreen_reading != applied.fullscreen_reading || draft.font != applied.font || draft.font_weight != applied.font_weight || draft.font_size_px != applied.font_size_px ||
            draft.line_height != applied.line_height || draft.margin_x_px != applied.margin_x_px ||
            draft.paragraph_gap_lines != applied.paragraph_gap_lines || draft.indent_chars != applied.indent_chars;
    }
    std::string chapter_label() const {
        if (txt_name(book_path) && document.metadata().txt_headings.empty()) return tr("正文");
        if (!document.metadata().txt_headings.empty()) {
            const auto current = current_toc();
            return current < document.toc().size() ?
                reading_footer::chapter_label(document.toc()[current].label, current) : tr("正文");
        }
        for (const auto &item : document.toc())
            if (item.resource == chapter.resource && !item.label.empty()) return reading_footer::chapter_label(item.label, chapter_index);
        return trf("第 {chapter} 章", {{"chapter", std::to_string(chapter_index + 1)}});
    }
    std::uint32_t total_pages() const {
        std::size_t total = 0;
        for (const auto &item : chapter_pages)
            for (const auto &starts : item) total += starts.size();
        return static_cast<std::uint32_t>(total);
    }
    std::uint32_t current_page() const {
        return current_page(page.start);
    }
    std::uint32_t current_page(std::size_t start) const {
        return counted_page(chapter_index, chapter.part, start);
    }
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
    std::pair<std::uint64_t, std::uint64_t> estimated_position(std::size_t end) const {
        return estimated_position(chapter_index, loaded_part(end));
    }
    sync::PartPosition loaded_part(std::size_t offset) const {
        return {chapter.part, static_cast<std::uint32_t>(offset), static_cast<std::uint32_t>(chapter.text.size()),
                chapter.last_part};
    }
    std::pair<std::uint64_t, std::uint64_t> estimated_position(std::size_t index, const sync::PartPosition &at) const;
    double estimated_fraction(std::size_t end) const {
        const auto [read, total] = estimated_position(end);
        return total ? static_cast<double>(read) / total : 0;
    }
    int book_percent_value(std::size_t start, std::size_t end) const {
        return book_percent_value(chapter_index, loaded_part(end), start);
    }
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
    std::string confirmed_book_position() const {
        const auto end = session.visible_record() ? confirmed_page_end : loaded_part(offset);
        const auto percent = std::to_string(book_percent_value(chapter_index, end, offset)) + "%";
        if (const auto current = counted_page(chapter_index, end.part, offset))
            return std::to_string(current) + " / " + std::to_string(total_pages()) + " · " + percent;
        if (pages_complete) return trf("共 {count} 页 · {percent}", {{"count", std::to_string(total_pages())}, {"percent", percent}});
        return percent;
    }
    std::string footer_position_text() const {
        switch (footer_position) {
        case reading_footer::Position::Hidden:
            return {};
        case reading_footer::Position::Chapters:
            if (!document.metadata().txt_headings.empty()) {
                const auto current = current_toc();
                const auto total = std::to_string(document.toc().size());
                return current < document.toc().size() ?
                    trf("第 {chapter} 章 / 共 {total} 章", {{"chapter", std::to_string(current + 1)}, {"total", total}}) : trf("正文 · 共 {total} 章", {{"total", total}});
            }
            return trf("第 {chapter} 章 / 共 {total} 章", {{"chapter", std::to_string(chapter_index + 1)}, {"total", std::to_string(document.spine().size())}});
        case reading_footer::Position::ChapterName:
            return chapter_label();
        case reading_footer::Position::Percent:
            if (const auto current = current_page())
                return reading_footer::percent(static_cast<double>(current) / total_pages());
            return reading_footer::percent(estimated_fraction(page.end), pages_complete);
        case reading_footer::Position::Pages:
            if (const auto current = current_page())
                return std::to_string(current) + " / " + std::to_string(total_pages());
            if (pages_complete) return trf("共 {count} 页", {{"count", std::to_string(total_pages())}});
            if (pages_failed) return tr("页数未知");
            return counting_blocked == chapter_index ? tr("等待统计页数") : tr("正在统计页数");
        }
        return {};
    }
    std::string sync_badge() const {
        if (txt_name(book_path)) return tr("仅本地");
        if (session.needs_save()) return tr("尚未保存");
        if (active_network) return tr("同步中");
        if (!badge_configured) return tr("未配置");
        const auto& config = badge_config;
        if (uncertain_record && uncertain_record->book_id == document.metadata().book_id && same_account(config, uncertain_config))
            return tr("待核实");
        if (acknowledged_record && session.saved_record() && *acknowledged_record == *session.saved_record() && same_account(config, acknowledged_config))
            return tr("已上传");
        return tr("待上传");
    }
    void reading_header(bool panel = false);
    static constexpr int bookmark_cell_left = 584;
    static constexpr int panel_strip = 56;
    static constexpr int typography_card_height = 88, typography_card_pitch = 100;
    static constexpr int typography_actions_top = 1128, typography_action_height = 72;
    static constexpr int panel_card_left = 24, panel_card_right = 348, panel_card_width = 312;
    int typography_card_rows() const { return txt_name(book_path) || !draft.original_epub ? 4 : 3; }
    int typography_cards_top() const {
        const int last_row_top = txt_name(book_path) || !draft.original_epub ? 1024 : 980;
        return last_row_top - (typography_card_rows() - 1) * typography_card_pitch;
    }
    int typography_style_top() const { return typography_cards_top() - 16 - 72; }
    int typography_tabs_top() const {
        return typography_style_top() - 16 - 100;
    }
    int typography_strip_top() const { return typography_tabs_top() - panel_strip; }
    void tabs(int top, int active) {
        const int ids[] = {Contents, Typography, Sync, Refresh};
        const char *names[] = {tr("目录"), tr("排版"), tr("同步"), tr("刷新模式")};
        canvas.fill_rect(0, top - panel_strip, 684, panel_strip + 100, 15);
        canvas.line(0, top - panel_strip, 683, top - panel_strip, 1, 3);
        const auto collapse_label = tr("收起");
        const int collapse_width = text_width(collapse_label, 24);
        text(collapse_label, 622 - collapse_width, top_for_middle(ui, 24, top - panel_strip / 2), 24, collapse_width, 3);
        canvas.line(634, top - 32, 641, top - 25, 3, 2);
        canvas.line(641, top - 25, 648, top - 32, 3, 2);
        hits.push_back({0, top - panel_strip, 684, panel_strip, Back});
        for (int i = 0; i < 4; ++i) {
            const int x = i * 171;
            if (ids[i] == active) canvas.fill_rect(x + 40, top + 86, 171 - 80, 3, 1);
            const auto label = ids[i] == Refresh && text_width(names[i], 28) > 155 ? tr("模式") : names[i];
            center(label, x, top_for_middle(ui, 28, top + 50), 171, 28, 1, ids[i] == active);
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
    void overlay(bool full = false) {
        display.present({{"ui", 0, 0, ++ui_sequence},
                         {},
                         FramePurpose::Overlay,
                         full ? RefreshIntent::FullClean : RefreshIntent::Content});
    }
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
    int page_bookmark_height() const { return settings.fullscreen_reading ? fullscreen_margin_y_px - 2 : 44; }
    int reading_touch_bottom() const { return settings.fullscreen_reading ? 1216 : footer_top; }
    int reading_status_top() const { return settings.fullscreen_reading ? fullscreen_notice_top : footer_top; }
    static int reading_top(const ReaderSettings& cfg) { return cfg.fullscreen_reading ? fullscreen_margin_y_px : 64; }
    graphics::LayoutSpec reading_layout(const ReaderSettings& cfg, bool return_bar = false) {
        const int bottom = cfg.fullscreen_reading ? 1216 - fullscreen_margin_y_px : footer_top;
        auto spec = layout(cfg, return_bar ? 144 : reading_top(cfg), bottom);
        spec.bleed_to_screen_top = cfg.fullscreen_reading && !return_bar;
        return spec;
    }
    graphics::LayoutSpec layout(const ReaderSettings &cfg, int top, int bottom) {
        graphics::LayoutSpec spec;
        spec.left = cfg.margin_x_px;
        spec.top = top;
        spec.width = 684 - 2 * spec.left;
        spec.height = bottom - top;
        spec.font_px = cfg.font_size_px;
        spec.original_epub = cfg.original_epub && !txt_name(book_path);
        spec.original_weight_adjustment = spec.original_epub ? cfg.original_weight_adjustment : 0;
        spec.justify = !spec.original_epub && cfg.justify;
        spec.limit_english_justification = spec.justify;
        spec.txt_reflow = cfg.txt_reflow && txt_name(book_path);
        body.set_pixel_size(spec.font_px);
        spec.indent_chars = cfg.indent_chars;
        spec.paragraph_gap = static_cast<int>(
            std::lround(cfg.paragraph_gap_lines * cfg.font_size_px * cfg.line_height));
        spec.line_height = std::max(static_cast<int>(std::lround(cfg.line_height * spec.font_px)),
                                    body.metrics().ascender + body.metrics().descender);
        return spec;
    }
    void load_fonts(const ReaderSettings *preview = nullptr);
    void load_library_font(const std::string& path);
    graphics::Font& library_text_font() { return library_uses_body ? body : library_font.valid() ? library_font : ui; }
    void scan_fonts() {
        fonts.clear();
        DIR *dir = opendir((root + "/Fonts").c_str());
        if (dir) {
            for (int visited = 0; visited < 2048 && fonts.size() < kMaxScannedFonts; ++visited) {
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
        const bool first_boot = store.needs_onboarding();
        last_clock_sync_at = store.last_clock_sync();
        refresh_badge_config();
        if (!ui.open_fallback()) return false;
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
        if (card_available) {
            for (const auto* directory : {"Fonts", "LockScreens", "Dictionaries"}) {
                int error_number = 0;
                store.create_top_level_directory(directory, error_number);
            }
        }
        draw_library();
        update_boot();
        if (first_boot && view == View::Library) start_onboarding(false);
        overlay(true);
        return true;
    }
    void scan_library();
    static constexpr int library_list_row_height = 110;
    bool library_resume() const { return (folder.empty() || folder_root) && (has_book() || !last_book.empty()); }
    std::size_t library_page_size() const { return library_grid || library_resume() ? 6 : 8; }
    std::size_t library_begin() const { return library_page * library_page_size(); }
    std::size_t library_pages() const {
        const auto size = library_page_size();
        return entries.empty() ? 1 : 1 + (entries.size() - 1) / size;
    }
    std::string recent_date(std::int64_t utc) const {
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        if (now < provision::clock_floor || utc < provision::clock_floor || utc > now) return "";
        const auto days = (now + 8 * 3600) / 86400 - (utc + 8 * 3600) / 86400;
        if (days == 0) return tr("今天");
        if (days == 1) return tr("昨天");
        if (days < 30) return trf("{count} 天前", {{"count", std::to_string(days)}});
        const auto shifted = static_cast<std::time_t>(utc + 8 * 3600);
        std::tm date{};
        if (!gmtime_r(&shifted, &date)) return "";
        char value[12]; std::strftime(value, sizeof(value), "%m-%d", &date); return value;
    }
    static std::string entry_title(const Entry &entry) {
        return entry.directory ? entry.name : entry.name.substr(0, entry.name.size() - (txt_name(entry.name) ? 4 : 5));
    }
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
    std::string position_header(const std::string &relative, const struct stat &info) const {
        return "PPPOS2\n" + std::string(i18n::language_code(settings.language)) + "\n" + relative + "\n" + std::to_string(info.st_size) + " " +
               std::to_string(static_cast<long long>(info.st_mtime)) + "\n";
    }
    void remember_position() {
        struct stat info{};
        if (!has_book() || !card_available || !session.visible_record() || page.end <= page.start ||
            stat((root + "/" + book_path).c_str(), &info) != 0) return;
        const auto name = position_name(book_path);
        auto bytes = position_header(book_path, info) + chapter_label() + "\n" + book_percent();
        if (session.needs_save()) {
            pending_position = PendingPosition{name, std::move(bytes), document.metadata().book_id, session.visible_record()->anchor};
            return;
        }
        pending_position.reset();
        store.save_cache(name, bytes);
    }
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
    void describe(Entry &entry);
    struct CoverEntry {
        std::string name, header;
        std::size_t bytes;
    };
    static CoverEntry cover_entry(const std::string &relative, const struct stat &info, int width,
                                  int height, bool crop) {
        char name[64];
        std::snprintf(name, sizeof(name), "covers/%016llx-%dx%d%s", path_hash(relative),
                      width, height, crop ? "c" : "");
        return {name, "PPCOVER4\n" + relative + "\n" + std::to_string(info.st_size) + " " +
                          std::to_string(static_cast<long long>(info.st_mtime)) + "\n",
                static_cast<std::size_t>((width + 1) / 2) * height};
    }
    bool cached_cover(const CoverEntry &entry, std::vector<std::uint8_t> &pixels) {
        std::string cached;
        if (store.load_cache(entry.name, entry.header.size() + entry.bytes, cached) != StoreStatus::Ok ||
            cached.compare(0, entry.header.size(), entry.header) != 0 ||
            cached.size() != entry.header.size() + entry.bytes)
            return false;
        pixels.assign(cached.begin() + static_cast<std::ptrdiff_t>(entry.header.size()), cached.end());
        return true;
    }
    std::optional<std::vector<std::uint8_t>> decode_cover(const std::string &relative, const CoverEntry &entry,
                                           int width, int height, bool crop) {
        const auto identity = sha256_file(root + "/" + relative);
        if (identity.empty()) return std::nullopt;
        graphics::Canvas thumbnail(width, height);
        std::vector<std::uint8_t> encoded;
        std::string detail;
        ContentFailureKind failure_kind;
        const bool read = EpubDocument::read_cover(root + "/" + relative, encoded, detail, {}, &failure_kind);
        const bool loaded = read &&
            graphics::render_image(thumbnail, encoded, 0, 0, width, height, detail, crop, {}, &failure_kind);
        if (!loaded && !content_failure_is_permanent(failure_kind)) return std::nullopt;
        if (!loaded && sha256_file(root + "/" + relative) != identity) return std::nullopt;
        store.save_cache(entry.name, loaded ? entry.header + std::string(thumbnail.pixels().begin(), thumbnail.pixels().end())
                                           : entry.header + cover_failure_payload(identity, failure_kind));
        return loaded ? thumbnail.pixels() : std::vector<std::uint8_t>{};
    }
    std::vector<std::uint8_t> cover_pixels(const std::string &relative, const struct stat &info,
                                           int width, int height, bool crop) {
        const auto entry = cover_entry(relative, info, width, height, crop);
        std::vector<std::uint8_t> pixels;
        return cached_cover(entry, pixels) ? pixels : decode_cover(relative, entry, width, height, crop).value_or(std::vector<std::uint8_t>{});
    }
    void blit(const std::vector<std::uint8_t> &pixels, int x, int y, int width, int height) {
        const int stride = (width + 1) / 2;
        for (int row = 0; row < height; ++row)
            for (int column = 0; column < width; ++column) {
                const auto packed = pixels[row * stride + column / 2];
                canvas.pixel(x + column, y + row, column % 2 ? packed >> 4 : packed & 15);
            }
    }
    static constexpr int kRowCoverWidth = 59, kRowCoverHeight = 80;
    static constexpr int kContinueCoverWidth = 96, kContinueCoverHeight = 128;
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
    Cover draw_cover(const Entry &entry, int x, int y, bool decode,
                     int width = kLibraryCoverWidth, int height = kLibraryCoverHeight);
    std::string library_location() const {
        return library_home_root.empty() ? tr("已设置的书库目录") : " " + library_home_root + "/";
    }
    std::string direct_copy_hint() {
        auto path = library_home_root;
        bool cut = false;
        const auto line = [&] { return tr("拷到卡上 ") + std::string(cut ? "…/" : "") + path + tr("/，插回后重启设备"); };
        for (auto slash = path.find('/'); text_width(line(), 22) > 554 && slash != std::string::npos; slash = path.find('/')) {
            path.erase(0, slash + 1);
            cut = true;
        }
        return path.empty() || text_width(line(), 22) > 554 ? tr("拷到卡上已设置的书库目录，插回后重启设备") : line();
    }
    static constexpr int kSectionGap = 56;
    void draw_first_use() {
        text(tr("书库里还没有书"), 36, 160, 40, 612, 1, true);
        text(tr("选一种方式放入 EPUB 或 TXT"), 36, 222, 26, 612, 3);
        canvas.line(24, 288, 660, 288, 1, 2);
        settings_row(UsbStart, tr("USB 连接电脑"), tr("电脑直接管理 TF 卡，退出时重启"), 288, 1);
        settings_row(WifiUpload, tr("Wi-Fi 传书"), tr("经设备热点，手机扫码上传，不需要路由器"), 420, 1);
        settings_row(0, tr("直接放入 TF 卡"), direct_copy_hint(), 552, 1);
        button(Help, tr("查看使用说明"), 24, 684 + kSectionGap, 636, 96, false, 30);
        wrapped(std::string(tr("内置字体只含 3,500 个常用字，生僻字会显示为方框。官网 ")) + kProjectHome +
                tr(" 可下载完整的文楷字体，放入 TF 卡 Fonts/，重启后在「排版」中选择。"), 36, 684 + kSectionGap + 96 + 40, 612, 22, true, 3);
    }
    void draw_library(bool decode = false, bool keep_cover_attempts = false);
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
        if (!seek(record->anchor, position, tr("已显示位置无法恢复")))
            return false;
        offset = position;
        return true;
    }
    bool recover_body() {
        const auto record = session.visible_record() ? session.visible_record() : session.saved_record();
        std::uint32_t position;
        if (async_content && record && (author_fonts_released || !chapter.offset_for(record->anchor, position)))
            return begin_seek(record->anchor);
        if (author_fonts_released && !async_content) {
            author_fonts = prepare_embedded_fonts(store, document, chapter, author_fonts_stable);
            author_fonts_released = false;
        }
        return restore_confirmed_position() && show_page(offset);
    }
    std::uint32_t restored_offset = 0;
    static bool memory_error(const std::string &detail) {
        return detail.rfind(kChapterMemoryError, 0) == 0;
    }
    std::string chapter_error(const std::string &detail) const {
        return memory_error(detail) ? tr("本章内容过大，设备内存不足，无法显示。") : trf("章节读取失败。{reason}。", {{"reason", reason(detail, tr("无法解析文件内容"))}});
    }
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
        author_fonts = prepare_embedded_fonts(store, document, chapter, author_fonts_stable);
        author_fonts_released = false;
        chapter_index = index;
        offset = 0;
        return true;
    }
    ChapterLoad reading_detail() const {
        return settings.original_epub && !txt_name(book_path) ? ChapterLoad::Original : ChapterLoad::Reading;
    }
    bool load_chapter(std::size_t index, std::uint32_t part = 0) {
        const StageTimer timer{"chapter_reading", 0};
        return load_with(index, [&](Chapter &loaded, std::string &detail) {
            return load_content(store, document, index, loaded, detail, reading_detail(), part);
        });
    }
    bool seek(const Anchor &anchor, std::uint32_t &position, const std::string &missing) {
        std::size_t index;
        if (!document.chapter_index_for(anchor.resource, index)) {
            error(missing);
            return false;
        }
        if (index == chapter_index && chapter.resource == anchor.resource && chapter.offset_for(anchor, position))
            return true;
        return load_with(index, [&](Chapter &loaded, std::string &detail) {
            return load_anchored_content(store, document, index, loaded, detail, reading_detail(), anchor, position);
        }, missing);
    }
    bool seek_fragment(std::size_t index, const std::string &fragment, std::uint32_t &position,
                       const std::string &missing) {
        position = 0;
        if (fragment.empty()) return load_chapter(index);
        const auto holds = [&](const Chapter &part) {
            std::uint32_t ignored;
            return part.fragment_offset(fragment, ignored);
        };
        if (!load_with(index, [&](Chapter &loaded, std::string &detail) {
                return find_content(store, document, index, loaded, detail, reading_detail(), holds);
            }, missing) || !chapter.fragment_offset(fragment, position))
            return false;
        if (position >= chapter.text.size() && !chapter.last_part) {
            position = 0;
            return load_chapter(index, chapter.part + 1);
        }
        return true;
    }
    void footer();
    void draw_battery_icon(int center_x, int center_y);
    std::size_t readable_offset(std::size_t start) const {
        return pico_page::readable_offset(chapter, start, settings.original_epub);
    }
    const DocumentImage* image_at(std::size_t start) const {
        return pico_page::image_at(chapter, start);
    }
    graphics::Page plan_page(std::size_t start, const graphics::LayoutSpec& spec) {
        return plan_page(chapter, start, spec);
    }
    graphics::Page plan_page(const Chapter &source, std::size_t start, const graphics::LayoutSpec& spec,
                             bool collect = true, const std::function<bool()>& stop = {}) {
        const StageTimer timer{"page_layout", 0};
        return plan_content(body, source, start, spec, collect, stop, &fonts_for(source));
    }
    const graphics::AuthorFonts& fonts_for(const Chapter& source) const {
        if (typography_preview && &source == &typography_preview->chapter) return typography_preview->fonts;
        return &source == &counting_chapter ? counting_fonts : author_fonts;
    }

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
        if (reading_detail() == ChapterLoad::Original && !author_fonts_stable) { pages_cache_disabled = true; return; }
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
        const auto spec = reading_layout(settings);
        char name[32], geometry[160];
        std::snprintf(name, sizeof(name), "%s/%016llx", spec.original_epub ? "pages-original" : "pages", path_hash(book_path));
        std::snprintf(geometry, sizeof(geometry), "%lld %lld\n%lld %lld\n%d %d %d %d %d %d %d %d %d %d %zu\n",
                      static_cast<long long>(book.st_size), static_cast<long long>(book.st_mtime),
                      static_cast<long long>(font.st_size), static_cast<long long>(font.st_mtime),
                      spec.left, spec.top, spec.width, spec.height, spec.font_px, spec.line_height,
                      spec.indent_chars, spec.paragraph_gap, spec.bleed_to_screen_top, spec.txt_reflow, spine.size());
        pages_name = name;
        const auto revision = spec.original_epub ? ":original-pagination-v6\n" :
            txt_name(book_path) ? ":txt-pagination-v7\n" : ":pagination-v6\n";
        pages_header = std::string("PPPAGES3\n") + revision_for(book_path, document.metadata()) + revision + book_path + "\n" + font_file + "\n" + geometry +
            document.metadata().book_id + "\n" + font_identity + "\n";
        if (spec.justify) pages_header += "justify\n";
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

    static std::size_t chapter_page_bytes(const PageIndices::value_type& parts) {
        PP_HOST_WORK(host_work::add(host_work::PageIndexVisits);)
        std::size_t bytes = parts.capacity() * sizeof(std::vector<std::uint32_t>);
        for (const auto& starts : parts) {
            PP_HOST_WORK(host_work::add(host_work::PageIndexVisits);)
            bytes += starts.capacity() * sizeof(std::uint32_t);
        }
        return bytes;
    }
    std::size_t page_index_bytes() const {
        std::size_t bytes = chapter_pages.capacity() * sizeof(decltype(chapter_pages)::value_type);
        for (const auto& parts : chapter_pages) bytes += chapter_page_bytes(parts);
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
    void save_pages() {
        if (async_content || !pages_dirty || pages_name.empty() || pages_cache_disabled) return;
        if (!write_page_cache(store, pages_name, pages_header, chapter_pages, counted)) pages_cache_disabled = true;
        pages_dirty = false;
    }
    static constexpr std::size_t cache_target = 4U * 1024 * 1024;
    std::uint64_t cache_evictions = 0;
    CacheUsage cache_usage(std::size_t page_index_size) const {
        CacheUsage usage;
        usage.body_bitmap = body_canvas ? body_canvas->pixels().capacity() : 0;
        if (next_page) usage.next_layout = next_page->glyphs.capacity() * sizeof(graphics::PositionedGlyph) +
            next_page->lines.capacity() * sizeof(graphics::TextLine);
        usage.next_bitmap = next_bitmap ? next_bitmap->pixels().capacity() : 0;
        usage.bitmap_hits = bitmap_hits;
        usage.next_chapter = prepared_chapter ? prepared_chapter_size_bytes : 0;
        usage.chapter_prefetch_hits = chapter_prefetch_hits;
        usage.chapter_stream = chapter_session ? chapter_stream_bytes : 0;
        usage.page_index = page_index_size;
        for (const auto& cover : covers) usage.cover_pixels += cover.pixels.capacity();
        if (detail_cover) usage.cover_pixels += detail_cover->pixels().capacity();
        if (custom_sleep_image) usage.cover_pixels += custom_sleep_image->pixels().capacity();
        if (sleep_cover_image) usage.cover_pixels += sleep_cover_image->pixels().capacity();
        usage.cover_pixels += sleep_wait_footer ? kSleepWaitFooterBytes : 0;
        usage.ui_glyph_bytes = ui.cached_glyph_bytes(); usage.body_glyph_bytes = body.cached_glyph_bytes();
        usage.ui_glyphs = ui.cached_glyphs(); usage.body_glyphs = body.cached_glyphs();
        usage.ui_advances = ui.cached_advances(); usage.body_advances = body.cached_advances();
        for (const auto& face : author_fonts.faces) if (face) {
            usage.body_glyph_bytes += face->cached_glyph_bytes();
            usage.body_glyphs += face->cached_glyphs();
            usage.body_advances += face->cached_advances();
        }
        usage.library_glyph_bytes = library_font.cached_glyph_bytes();
        usage.library_glyphs = library_font.cached_glyphs(); usage.library_advances = library_font.cached_advances();
        usage.evictions = cache_evictions; usage.target = cache_target;
        usage.retained = (chapter_cache ? chapter_cache->bytes.capacity() : 0) +
            (recent_chapter_cache ? recent_chapter_cache->bytes.capacity() : 0) + usage.next_chapter + usage.chapter_stream +
            usage.body_bitmap + usage.next_layout + usage.next_bitmap +
            usage.page_index + usage.cover_pixels + usage.ui_glyph_bytes + usage.body_glyph_bytes + usage.library_glyph_bytes +
            (usage.ui_advances + usage.body_advances + usage.library_advances) * (sizeof(std::uint64_t) + sizeof(int));
        return usage;
    }
    CacheUsage cache_usage() const { return cache_usage(page_index_bytes()); }
    void reclaim_caches();
    bool background_content_ready() const {
        return !sleep_cover_deadline && !power_menu_wait && !session.needs_save() && view == View::Reading && !image_viewing && has_book() && card_available && !usb_prepared &&
               !display.busy() && !deferred && !save_job && !bookmark_job && !foreground_job && !pending_content && !body_images_pending &&
               !cover_job && !pending_covers;
    }
    bool chapter_cache_due() const {
        return chapter_cache_write_needed && chapter_cache && background_content_ready() && !counting_job;
    }
    void write_chapter_cache() {
        ContentJob::Request request;
        request.kind = ContentJob::Kind::ContentCacheWrite;
        request.root = root; request.generation = counting_generation;
        request.content_cache = chapter_cache;
        chapter_cache_write_needed = false;
        counting_job = std::make_unique<ContentJob>(std::move(request));
        if (!counting_job->start()) counting_job.reset();
    }
    bool counting_due() const {
        return background_content_ready() &&
               (!pages_complete || (!pages_cache_disabled && (pages_cache_pending || pages_dirty || (async_content && font_identity.empty())))) && !pages_failed &&
               (counting_blocked != chapter_index || std::chrono::steady_clock::now() >= counting_retry);
    }
    void release_counting() {
        if (counting_job) counting_job->cancel();
        else if (counting_session) {
            release(counting_session->chapter); counting_session->author_fonts.clear(); counting_session->loaded = false;
        }
        release(counting_chapter);
        counting_fonts.clear();
        counting_loaded = false;
    }
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
    void count_pages(const std::function<bool()>& stop);
    void draw_image_error(const graphics::ImageBox& box) {
        outline(box.x, box.y + 12, box.width, box.height - 24, 2);
        center(tr("图片无法显示"), box.x + 8, box.y + box.height / 2 - 14, box.width - 16, 28);
    }
    bool draw_page_content(const graphics::Page& planned, const graphics::LayoutSpec& spec, bool progressive = false,
                           const graphics::AuthorFonts* fonts = nullptr);
    bool body_reusable(std::size_t start, const graphics::LayoutSpec& spec) const {
        return body_canvas && start == page.start && same_layout(body_spec, spec);
    }
    bool render_body(std::size_t start);
    bool show_page(std::size_t start, bool reverse_page_effect = false, PageTurnOrigin origin = PageTurnOrigin::Middle);
    void remember_page() {
        confirmed_page_end = loaded_part(page.end);
        if (author_fonts_stable) record_boundary(page_history, page.start, page.end);
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
        if (i == starts.begin() || (limit != chapter.text.size() && (i == starts.end() || *i != limit)))
            return std::nullopt;
        return *std::prev(i);
    }
    bool paginate(std::size_t limit, std::size_t &previous) {
        if (const auto cached = previous_page(limit)) { previous = *cached; return true; }
        const auto spec = reading_layout(settings, link_return.has_value());
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
    void show_page_before(std::size_t limit, PageTurnOrigin origin) {
        std::size_t previous = 0;
        if (!paginate(limit, previous)) {
            error(tr("上一页无法排版"));
            return;
        }
        show_page(previous, true, origin);
    }
    void turn_back_to(std::size_t limit, PageTurnOrigin origin) {
        if (const auto cached = previous_page(limit)) { show_page(*cached, true, origin); return; }
        if (async_content) { begin_chapter(chapter_index, chapter.part, ContentJob::Target::Before, limit, origin); return; }
        constexpr std::size_t announced_scalars = 10000;
        if (limit > announced_scalars)
            defer(tr("正在排版…"), [this, limit, origin] { show_page_before(limit, origin); });
        else
            show_page_before(limit, origin);
    }
    static constexpr std::uint32_t announced_bytes = 150000;
    void change_part(std::size_t index, std::function<void()> work) {
        if (document.spine()[index].bytes > announced_bytes) defer(tr("正在排版…"), std::move(work));
        else work();
    }
    void turn(bool forward, PageTurnOrigin origin);
    void expire_sync_return() {
        if (!sync_return) return;
        link_return.reset();
        sync_return = false;
    }
    static constexpr int kToolbarIds[] = {Contents, Typography, Sync, Refresh, FullRefresh};
    static constexpr const char* kToolbarLabels[] = {"目录", "排版", "同步", "刷新模式", "全刷"};
    void draw_toolbar();
    void draw_tools();
    std::size_t current_toc() const {
        const auto &toc = document.toc();
        std::size_t current = toc.size();
        const auto& txt_headings = document.metadata().txt_headings;
        if (!txt_headings.empty()) {
            const auto next = std::upper_bound(txt_headings.begin(), txt_headings.end(),
                std::make_pair(chapter.part, offset), [](const auto& position, const TxtHeading& heading) {
                    return position.first < heading.part ||
                        (position.first == heading.part && position.second < heading.offset);
                });
            return next == txt_headings.begin() ? toc.size() : static_cast<std::size_t>(next - txt_headings.begin() - 1);
        }
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
    int toc_percent(std::size_t index) const {
        const auto &toc = document.toc();
        const auto &spine = document.spine();
        const auto& txt_headings = document.metadata().txt_headings;
        if (index < txt_headings.size()) {
            const auto& heading = txt_headings[index];
            return spine.empty() || !spine[0].bytes ? -1 :
                static_cast<int>(static_cast<std::uint64_t>(heading.byte_offset) * 100 / spine[0].bytes);
        }
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
    void contents_switch(int top) {
        canvas.fill_rect(0, top, 684, contents_switch_height, 15);
        const auto count = bookmarks_usable() && !bookmarks.bookmarks.empty()
                               ? " " + std::to_string(bookmarks.bookmarks.size()) : std::string();
        const std::string names[] = {tr("目录"), tr("书签") + count};
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
    void draw_font_picker();
    void cancel_font_picker() {
        abandon_content();
        draft.font = font_entry;
        draft.font_weight = font_entry_weight;
        reset_overrides = font_entry_reset;
        font_picker_open = false;
        draw_typography(false);
    }
    void font_picker_control(int id);
    void draw_typography(bool prepare_images = true);
    void change_epub_layout();
    std::string weight_name(const std::string &weight) const {
        return weight == "light" ? tr("细") : weight == "medium" ? tr("中粗") : weight == "bold" ? tr("粗") : tr("常规");
    }
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
    std::string font_name(const std::string &font) const {
        return font.find("WenKai") != std::string::npos     ? tr("文楷")
               : font.find("NotoSans") != std::string::npos ? tr("思源黑体")
                                                            : basename(font);
    }
    static std::string decimal(double value) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.2f", value);
        return buf;
    }
    static std::string compact(double value) {
        auto shown = decimal(value);
        shown.erase(shown.find_last_not_of('0') + 1);
        if (shown.back() == '.') shown.pop_back();
        return shown;
    }
    std::string refresh_name(const ReaderSettings &values) const {
        return values.refresh_preset == "clear" ? tr("清晰") : values.refresh_preset == "fast" ? tr("快速") : tr("均衡");
    }
    std::string sleep_name(const ReaderSettings &values) const {
        return values.sleep_screen == "custom" ? tr("自定义图片") : values.sleep_screen == "reading" ? tr("保留正文") : values.sleep_screen == "simple" ? tr("简洁屏保") : tr("书籍封面");
    }
    static int refresh_interval(const ReaderSettings &values) {
        return values.refresh_preset == "fast" ? values.fast_full_refresh_pages : values.balanced_full_refresh_pages;
    }
    std::string refresh_interval_label(const ReaderSettings &values) const {
        const auto pages = refresh_interval(values);
        if (pages == 1) return tr("每页");
        return pages == 0 ? tr("不自动全刷") : trf("每 {count} 页", {{"count", std::to_string(pages)}});
    }

    int draw_language_choices() {
        const i18n::Language languages[] = {i18n::Language::SimplifiedChinese, i18n::Language::TraditionalChinese, i18n::Language::English};
        const int actions[] = {LanguageSimplified, LanguageTraditional, LanguageEnglish};
        for (int index = 0; index < 3; ++index)
            choice(actions[index], i18n::language_name(languages[index]), "", 160 + index * 136, settings.language == languages[index]);
        return 576 + wrapped(tr("只更改界面语言，书籍正文、书名和文件名保持原文。"), 36, 576, 612, 24);
    }
    void choice_mark(int cx, int cy, bool selected, std::uint8_t gray = 1) {
        for (int row = -18; row <= 18; ++row)
            for (int col = -18; col <= 18; ++col) {
                const int radius = row * row + col * col;
                if ((radius <= 324 && radius >= 225) || (selected && radius <= 100))
                    canvas.pixel(cx + col, cy + row, gray);
            }
    }
    void panel_choice_card(int id, const char* label, int x, int y, bool selected, bool bold = false) {
        constexpr int height = 72, mark_size = 36, gap = 14;
        const auto gray = static_cast<std::uint8_t>(id ? 1 : 5);
        outline(x, y, panel_card_width, height, 2, gray);
        const int label_width = text_width(label, 28);
        const int mark_left = x + (panel_card_width - mark_size - gap - label_width) / 2;
        choice_mark(mark_left + mark_size / 2, y + height / 2, selected, gray);
        text(label, mark_left + mark_size + gap, top_for_middle(ui, 28, y + height / 2, bold),
             28, label_width + 4, gray, bold);
        if (id) hits.push_back({x, y, panel_card_width, height, id});
    }
    void panel_navigation_card(int id, const std::string& label, const std::string& value,
                               int x, int y, int width) {
        constexpr int zone_width = 64, height = typography_card_height;
        const auto gray = static_cast<std::uint8_t>(id ? 1 : 5);
        outline(x, y, width, height, 2, gray);
        const int divider = x + width - zone_width;
        canvas.fill_rect(divider, y, 2, height, gray);
        text(label, x + 20, top_for_middle(ui, 24, y + 28), 24, width - zone_width - 28, id ? 3 : 5);
        text(value, x + 20, top_for_middle(ui, 28, y + 60, true), 28, width - zone_width - 28, gray, true);
        center("›", divider, top_for_middle(ui, 32, y + height / 2), zone_width, 32, gray);
        if (id) hits.push_back({x, y, width, height, id});
    }
    void choice(int id, const std::string &title, const std::string &description, int y, bool selected,
                bool pending_choice = false, bool show_save_state = true) {
        const bool single_line = description.empty();
        const int middle = y + (single_line ? 58 : 48);
        choice_mark(62, middle, selected);
        const int title_width = selected && show_save_state
            ? std::min(426, 648 - text_width(pending_choice ? tr("待保存") : tr("当前"), 24) - 16 - 102) : 540;
        text(title, 102, single_line ? top_for_middle(ui, 28, middle, true) : y + 16, 28, title_width, 1, true);
        if (!single_line) text(description, 102, y + 65, 24, 540, 3);
        if (selected && show_save_state) {
            const auto state = pending_choice ? tr("待保存") : tr("当前");
            if (single_line) {
                const int width = text_width(state, 24);
                text(state, 648 - width, top_for_middle(ui, 24, middle), 24, width);
            } else center(state, 540, y + 20, 108, 24);
        }
        hits.push_back({24, y, 636, 116, id});
    }
    void file_choice(int id, const std::string &name, const std::string &description, int y, bool selected,
                     const char *state = nullptr) {
        const int height = description.empty() ? 90 : 116;
        const int name_width = state ? 444 : 552;
        if (description.empty()) {
            const bool multiline = text_width(name, 28) > name_width;
            title_lines(name, 36, y + (multiline ? 10 : 28), name_width, 28, false, 2);
        } else {
            text(name, 36, y + 18, 28, name_width);
            text(description, 36, y + 62, 24, name_width, 3);
        }
        if (state) text(state, 500, y + height / 2 - 13, 24, 100, 3);
        for (int dy = -18; dy <= 18; ++dy)
            for (int dx = -18; dx <= 18; ++dx) {
                const int radius = dx * dx + dy * dy;
                if ((radius <= 324 && radius >= 256) || (selected && radius <= 144))
                    canvas.pixel(624 + dx, y + height / 2 + dy, 1);
            }
        canvas.line(36, y + height - 1, 648, y + height - 1, 12, 2);
        hits.push_back({24, y, 636, height, id});
    }
    bool panel_background(int top, bool live = true) {
        hits.clear();
        if (has_book()) {
            load_fonts();
            if (!render_body(offset)) return false;
            for (const auto &line : page.lines)
                if ((line.top < 104 && line.bottom > 104) || (line.top < top && line.bottom > top))
                    canvas.fill_rect(0, line.top, 684, line.bottom - line.top + 1, 15);
            if (live) reading_header(true);
            else header(document.metadata().title, 0, "", 660, &body);
        } else {
            canvas.clear();
            header(tr("阅读"), Back, tr("设置"));
        }
        canvas.fill_rect(0, top, 684, 1216 - top, 15);
        return true;
    }
    void draw_reading_settings();
    void draw_refresh() {
        if (refresh_from_settings) { draw_reading_settings(); return; }
        view = View::Refresh;
        if (!panel_background(502 - panel_strip)) return;
        tabs(502, Refresh);
        const auto &values = edit_snapshot.config.settings;
        choice(RefreshClear, tr("清晰"), tr("每页执行全刷，减少残影"), 616, values.refresh_preset == "clear");
        choice(RefreshBalanced, tr("均衡"), tr("灰阶更新，定期全刷"), 752, values.refresh_preset == "balanced");
        choice(RefreshFast, tr("快速"), tr("优先响应速度，可能更易留痕"), 884, values.refresh_preset == "fast");
        button(FullRefresh, tr("立即全刷"), 36, 1020, 294, 96);
        button(PageEffects, tr("翻页效果"), 354, 1020, 294, 96);
        if (values.refresh_preset == "clear") note(tr("自动全刷间隔：每页"), 1150);
        else {
            text(tr("自动全刷间隔"), 36, 1150, 24, 360);
            const auto interval = refresh_interval_label(values) + " ›";
            const int interval_width = text_width(interval, 24);
            text(interval, 660 - interval_width, 1150, 24, interval_width);
            hits.push_back({24, 1128, 636, 88, RefreshInterval});
        }
    }
    void draw_page_effects() {
        view = View::PageEffects;
        hits.clear(); canvas.clear(); header(tr("翻页效果"), Back, refresh_from_settings ? tr("阅读") : tr("刷新"));
        text(tr("实验功能 · 本次启动有效"), 36, 144, 26, 612, 3);
        for (int index = 0; index < static_cast<int>(std::size(page_effect_choices)); ++index) {
            const auto& option = page_effect_choices[index];
            choice(EffectOff + index, tr(option.title), tr(option.description),
                   220 + index * 136, page_effect == option.effect);
        }
        wrapped(tr("仅用于正文非全刷更新。清晰模式每页全刷，不播放效果。"), 36, 936, 612, 25, 3);
        wrapped(tr("效果、残影和灰阶均待真机验证。按全刷键可清理当前画面。"), 36, 1040, 612, 25, 3);
    }
    void draw_refresh_confirm() {
        draw_refresh();
        if (view == View::Error) return;
        view = View::RefreshConfirm;
        hits.clear();
        if (has_book() && !refresh_from_settings) header(document.metadata().title, 0, "", 660, &body);
        else header(tr("阅读"), 0);
        canvas.fill_rect(0, 752, 684, 464, 15);
        canvas.line(0, 752, 683, 752, 1, 3);
        text(tr("改用快速刷新？"), 36, 788, 32, 612, 1, true);
        wrapped(tr("快速模式可能减少灰阶或更易留下残影。可随时按 K2 全刷当前画面。"), 36, 858, 612, 26);
        note(tr("确认前保留当前刷新模式。"), 996);
        button(Back, trf("保持{mode}", {{"mode", refresh_name(edit_snapshot.config.settings)}}), 24, 1080, 306, 96);
        button(RefreshAccept, tr("改用快速"), 354, 1080, 306, 96, true);
    }
    void draw_interval() {
        view = View::RefreshInterval;
        hits.clear(); canvas.clear(); header(tr("自动全刷间隔"), Back, refresh_from_settings ? tr("阅读") : tr("刷新"));
        text(trf("{mode}模式", {{"mode", refresh_name(draft)}}), 36, 170, 32, 612, 1, true);
        wrapped(tr("间隔越短，清理残影越频繁。此设置不影响 K2 手动全刷。"), 36, 246, 612, 26);
        constexpr int stepper_top = 440, stepper_height = 96, value_size = 34;
        button(IntervalLess, "−", 36, stepper_top, 120, stepper_height);
        center(refresh_interval_label(draft), 168, top_for_middle(ui, value_size, stepper_top + stepper_height / 2), 348, value_size);
        button(IntervalMore, "+", 528, stepper_top, 120, stepper_height);
        note(tr("可设为 0 至 30 页，0 表示不自动全刷。"), 582);
        button(Back, tr("取消"), 24, 1080, 306, 96);
        button(IntervalSave, tr("保存间隔"), 354, 1080, 306, 96, true);
    }
    static constexpr int settings_pitch = 104;
    void settings_row(int id, const std::string &title, const std::string &value, int y, std::uint8_t rule = 12,
                      int height = 132) {
        const int lift = (132 - height) / 2;
        text(title, 36, y + 20 - lift, 28, 554, 1, true);
        text(value, 36, y + 70 - lift, 24, 554, 3);
        canvas.line(36, y + height - 1, 647, y + height - 1, rule, 2);
        if (!id) return;
        text("›", 636, y + 42 - lift, 28, 24);
        hits.push_back({24, y, 636, height, id});
    }
    bool wifi_enabled() const {
        if (portal_live || network_worker_id || update_worker_id) return true;
        if (bookmark_job) return false;
        const auto cfg = store.load_settings();
        return cfg.diagnostics.empty() && cfg.settings.wifi_enabled;
    }
    std::string quoted_name(const std::string& name) const {
        return settings.language == i18n::Language::English ? "\"" + name + "\"" : "「" + name + "」";
    }
    bool wifi_online() const {
        return wifi_status.phase == WifiPhase::Connected || wifi_status.phase == WifiPhase::Clock ||
               wifi_status.phase == WifiPhase::Request;
    }
    std::string wifi_summary() const {
        if (!wifi_enabled()) return tr("Wi-Fi 已关闭");
        if (portal_live && !portal_station) return portal_closing ? tr("正在结束…") : tr("设备热点已开启");
        switch (wifi_status.phase) {
        case WifiPhase::Connecting:
            return wifi_status.ssid.empty() ? tr("正在连接…") : trf("正在连接{network}", {{"network", quoted_name(wifi_status.ssid)}});
        case WifiPhase::Clock:
        case WifiPhase::Request:
        case WifiPhase::Connected: return trf("已连接{network}", {{"network", quoted_name(wifi_status.ssid)}});
        case WifiPhase::Hotspot: return tr("设备热点已开启");
        case WifiPhase::Closing: return tr("正在结束…");
        default: return tr("未连接");
        }
    }
    std::string network_label(std::string_view pinned_ssid) const {
        if (wifi_online()) return trf("已连接{network}", {{"network", quoted_name(wifi_status.ssid)}});
        return pinned_ssid.empty() ? tr("自动") : tr("指定 ") + std::string(pinned_ssid);
    }
    std::string network_label() const {
        if (wifi_online()) return network_label("");
        std::optional<provision::SavedWifi> pinned;
        std::string detail;
        if (!provision::load_selected_network(root, pinned, detail)) return tr("配置无效");
        return network_label(pinned ? pinned->ssid : "");
    }
    WifiTargets wifi_targets(std::string& problem, bool automatic = false) const {
        WifiTargets targets;
        std::vector<provision::SavedWifi> saved;
        std::optional<provision::SavedWifi> pinned;
        std::string detail;
        if (!provision::load_networks(root, saved, detail) || !provision::load_selected_network(root, pinned, detail))
            problem = tr("网络配置无效，请检查 PicoPage/network.toml。");
        else if (saved.empty()) problem = tr("还没有保存 Wi-Fi 网络，请先在 Wi-Fi 设置中添加。");
        else if (pinned && !automatic) targets.networks.push_back(*pinned);
        else {
            targets.automatic = true;
            targets.networks.assign(saved.begin(), saved.end());
        }
        return targets;
    }
    void wifi_link(int y) {
        text(tr("Wi-Fi 状态 ›"), 432, y, 24, 216);
        hits.push_back({420, y - 12, 240, 52, WifiSettings});
    }
    void draw_wifi();
    bool clock_running() const { return wifi_worker && wifi_kind == WifiOperation::Kind::Clock; }
    void start_clock_sync();
    bool wifi_free(bool allow_link = false) {
        if (wifi_worker || portal_live || active_network) wifi_notice = tr("请先结束当前任务。");
        else if (!wifi_enabled()) wifi_notice = tr("请先打开 Wi-Fi。");
        else if (!allow_link && (network::live(manual_hold) || wifi_online())) wifi_notice = tr("请先断开 Wi-Fi。");
        else return true;
        return false;
    }
    void start_wifi(WifiOperation::Kind kind, WifiTargets targets, network::Hold& hold) {
        network::release(hold);
        hold = std::make_shared<std::atomic_bool>(false);
        wifi_worker = ++wifi_sequence;
        if (&hold == &manual_hold) manual_target = targets;
        wifi_kind = kind;
        if (kind == WifiOperation::Kind::Clock) clock_from_sync = view == View::Sync;
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
    void join_wifi() {
        entry.field = TextEntry::Field::None;
        join_unsaved = false;
        join_failure = LinkFailure::Other;
        wifi_page = WifiPage::JoinFailed;
        if (!provision::valid_wifi(join_draft)) join_error = tr("网络名称或密码的格式不正确。");
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
    void leave_join(WifiPage page) {
        std::fill(join_draft.password.begin(), join_draft.password.end(), '\0');
        join_draft = {};
        entry = {};
        wifi_page = page;
    }
    void join_complete(const std::string& error, LinkFailure failure) {
        join_unsaved = false;
        if (!error.empty()) {
            join_error = error; join_failure = failure == LinkFailure::None ? LinkFailure::Other : failure;
            wifi_page = WifiPage::JoinFailed;
            return;
        }
        std::string detail;
        if (!provision::save_network_edit(root, join_draft, false, std::time(nullptr), detail)) {
            join_error.clear(); join_failure = LinkFailure::Other; join_unsaved = true;
            wifi_page = WifiPage::JoinFailed;
            return;
        }
        refresh_badge_config();
        std::fill(join_draft.password.begin(), join_draft.password.end(), '\0');
        join_draft.password.clear();
        wifi_page = WifiPage::Joined;
    }

    static constexpr int keyboard_top = 796;
    static constexpr RefreshRegion entry_field_region{0, 104, 684, 416}, entry_keys_region{0, keyboard_top, 684, 1216 - keyboard_top};
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
        case TextEntry::Field::WifiPassword: return 64;
        case TextEntry::Field::Endpoint: return 1024;
        default: return 256;
        }
    }
    bool entry_ready() const {
        if (entry.field == TextEntry::Field::WifiPassword)
            return entry.value.empty() ? !join_secured
                                       : entry.value.size() >= 8 && provision::valid_wifi({"-", entry.value});
        if (entry.field == TextEntry::Field::WifiName) return !entry.value.empty();
        return true;
    }
    std::vector<Key> keyboard() const;
    void draw_entry();
    void open_entry(TextEntry::Field field, const std::string& value) {
        entry = {};
        entry.field = field;
        entry.value = value;
        draw_entry(); overlay();
    }
    bool key_input(const AppInput& input);
    void flush_entry() {
        if (view != View::TextEntry || display.busy() || (!entry.field_dirty && !entry.keys_dirty)) return;
        const bool field = entry.field_dirty, keys = entry.keys_dirty || entry.enter_key_drawn_enabled != entry_ready();
        draw_entry();
        display.present({{"ui", 0, 0, ++ui_sequence}, {}, FramePurpose::Overlay, RefreshIntent::Content,
                         field && keys ? RefreshRegion{0, 104, 684, 1112} : field ? entry_field_region : entry_keys_region});
    }
    void entry_done();
    enum class UpdatePage { Home, Checking, Notes, Schema, Downloading, Failed, Ready, Result };
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
    std::uint64_t update_sequence = 0, update_request_id = 0, update_worker_id = 0;
    bool update_cancelling = false, update_ready = false, update_rolled_back = false, battery_charging = false;
    bool update_layout_supported = true;
    bool update_fresh = false;
    bool update_confirming = false;
    UpdateFailure update_failure = UpdateFailure::None;
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
    static constexpr int notes_top = 132, notes_first_top = 184, notes_bottom = 984;
    void load_update_state() {
        std::string cached;
        update_state = {};
        update_fresh = false;
        if (update_layout_supported && card_available && store.load_cache(update::state_cache, update::state_limit, cached) == StoreStatus::Ok)
            update::decode_state(cached, update_state);
    }
    bool save_update_state() {
        const auto encoded = update::encode_state(update_state);
        return card_available && !encoded.empty() && store.save_cache(update::state_cache, encoded) == StoreStatus::Ok;
    }
    update::Offer update_offer() const {
        return update_layout_supported && update_state.manifest
            ? update::evaluate(*update_state.manifest, running_version, update_state.skipped) : update::Offer::Current;
    }
    static bool update_installable(update::Offer offer) {
        return offer == update::Offer::Newer || offer == update::Offer::NewerSchema || offer == update::Offer::Development;
    }
    bool update_waiting() const { return update_request || update_worker_id; }
    std::string update_battery_block() const {
        if (battery_charging || (battery_percent && *battery_percent >= 20)) return {};
        return battery_percent ? trf("电量 {percent}%。充电或电量到 20% 以上再安装。", {{"percent", std::to_string(*battery_percent)}})
                               : tr("电量未知。请确认设备正在充电后再安装。");
    }
    std::string update_block() const {
        if (const auto battery = update_battery_block(); !battery.empty()) return battery;
        if (active_network || portal_live || wifi_worker || content_loading || foreground_job || save_job || bookmark_job)
            return tr("请先结束正在进行的同步、传书或书籍加载。");
        return {};
    }
    static int note_line_height(int size) { return size * 31 / 20; }
    void layout_notes(const std::string& markdown, bool truncated, int first_top);
    void draw_note_lines(std::size_t screen, int top, int bottom = 0) {
        const auto begin = note_starts[screen];
        const auto end = screen + 1 < note_starts.size() ? note_starts[screen + 1] : note_lines.size();
        int y = top;
        for (auto i = begin; i < end; ++i) {
            const auto& line = note_lines[i];
            if (i != begin && line.block_rows) y += line.gap;
            if (bottom && y + note_line_height(line.size) > bottom) { text("…", 36, y, 24, 612, 3); return; }
            ui.set_pixel_size(line.size);
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
    std::string update_stage(const std::string& version) const {
        return version.find("-beta.") != std::string::npos ? tr("测试版") : version.find("-rc.") != std::string::npos ? tr("候选版") : tr("正式版");
    }
    std::string update_checked() const {
        const auto day = recent_date(update_state.checked_at);
        const auto shifted = static_cast<std::time_t>(update_state.checked_at + 8 * 3600);
        std::tm time{};
        if (day.empty() || !gmtime_r(&shifted, &time)) return tr("上次检查时间未知");
        char value[8];
        std::strftime(value, sizeof(value), "%H:%M", &time);
        return tr("上次检查 ") + day + " " + value;
    }
    void update_head(const std::string& label, const std::string& version, const std::string& detail = "") {
        text(label, 36, 136, 24, 612, 3);
        text(version, 36, 176, version.size() > 18 ? 32 : 44, 612, 1, true);
        if (!detail.empty()) text(detail, 36, 244, 24, 612, 3);
    }
    void update_steps(int active) {
        static constexpr const char* names[] = {"1 下载", "2 校验", "3 重启"};
        outline(36, 452, 612, 72);
        for (int i = 0; i < 3; ++i) {
            const int x = 36 + i * 204;
            if (i) canvas.line(x, 452, x, 523, 1, 2);
            if (i == active) outline(x, 452, 204, 72, 5);
            center(std::string(tr(names[i])) + (i >= active ? "" : ui.contains(U'✓') ? " ✓" : tr(" 完成")), x, 474, 204, 24,
                   i == active ? 1 : 3, i == active);
        }
    }
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
        const auto label = offer == update::Offer::Development ? tr("换成发布构建") : tr("下载并安装");
        if (update_installable(offer) && update_block().empty())
            button(UpdateStart, label, 24, y, 636, 96, offer != update::Offer::Development);
        else {
            disabled_button(label, 24, y, 636, 96);
            if (update_installable(offer)) {
                outline(24, y, 636, 96);
                hits.push_back({24, y, 636, 96, UpdateStart});
            }
        }
    }
    enum class CellState : std::uint8_t { Done, Active, Failed, Waiting };
    void check_cell(int x, int y, const char* label, CellState state, const std::string& value, const std::string& note = "") {
        static constexpr int w = 196, h = 176;
        const std::uint8_t ink = state == CellState::Waiting ? 5 : 1;
        outline(x, y, w, h, state == CellState::Failed ? 3 : 2, ink);
        if (state == CellState::Failed) {
            canvas.fill_rect(x + w - 44, y + 14, 30, 30, 1);
            center("!", x + w - 44, y + 14, 30, 24, 15, true);
        }
        text(label, x + 20, y + 20, 24, state == CellState::Failed ? w - 72 : w - 40, ink);
        text(value, x + 20, y + 68, 28, w - 40, ink, true);
        if (!note.empty()) text(note, x + 20, y + 126, 24, w - 40, ink);
    }
    void check_cells(int y, CheckStage failed, const std::string& ssid, const std::string& note) {
        const bool wifi = failed == CheckStage::Wifi, internet = failed == CheckStage::Internet;
        check_cell(36, y, "Wi-Fi", wifi ? CellState::Failed : CellState::Done, wifi ? tr("未连接") : tr("已连接"), wifi ? note : ssid);
        if (wifi) check_cell(244, y, tr("互联网"), CellState::Waiting, tr("未进行"));
        else check_cell(244, y, tr("互联网"), internet ? CellState::Failed : CellState::Done, internet ? note : tr("正常"));
        if (failed != CheckStage::Server) check_cell(452, y, tr("更新服务器"), CellState::Waiting, tr("未进行"));
        else check_cell(452, y, tr("更新服务器"), CellState::Failed, tr("异常"), note);
    }
    std::string update_checked_at() const {
        const auto checked = update_checked();
        return checked.rfind(tr("上次检查 "), 0) == 0 ? checked.substr(std::string(tr("上次检查 ")).size()) : "";
    }
    void draw_update_home(update::Offer offer, int preview_bottom = 700);
    void draw_update();
    void update_begin(UpdateOperation::Kind kind);
    void update_cancel() {
        network::release(update_hold);
        update_confirming = false;
        if (update_request) {
            update_request.reset();
            update_request_id = 0;
            update_page = UpdatePage::Home;
        } else if (update_worker_id) update_cancelling = true;
    }
    void update_progress(std::uint64_t id, std::uint32_t bytes) {
        if (!id || id != update_worker_id || id != update_request_id || update_cancelling || display.busy() ||
            view != View::Update || update_page != UpdatePage::Downloading || !update_state.manifest) return;
        const auto total = update_state.manifest->size;
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
    void update_open() {
        if (view != View::Settings || !bookmark_io_ready()) return;
        message.clear(); update_notice.clear();
        if (update_check.stage != CheckStage::None) update_notice = update_check.body;
        if (!update_waiting()) {
            if (!update_ready && !update_fresh) load_update_state();
            update_page = UpdatePage::Home;
            note_lines.clear(); note_page = 0;
        }
        draw_update(); overlay();
    }
    bool update_control(int id);
    void draw_installing() {
        view = View::PowerOff;
        hits.clear(); canvas.clear();
        text(tr("正在安装"), 72, 430, 24, 540, 3);
        text(update_to, 72, 474, update_to.size() > 18 ? 34 : 48, 540, 1, true);
        canvas.line(72, 566, 611, 566, 1, 3);
        text(tr("请勿断电"), 72, 598, 34, 540, 1, true);
        wrapped(tr("屏幕会停在这一页，直到新固件画出首屏。如果安装没有完成，设备会自动回到原来的版本。"), 72, 664, 540, 26);
    }
    void draw_settings();
    void draw_language_settings();
    bool save_language(const SettingsSnapshot& snapshot, i18n::Language language, bool onboarding_pending = false);
    void draw_about() {
        view = View::About;
        hits.clear(); canvas.clear(); header(tr("关于"), Back, tr("设置"));
        text("Pico Page", 36, 144, 44, 612, 1, true);
        text(tr("Read Pico 墨水屏阅读固件"), 36, 206, 22, 612, 3);
        canvas.line(24, 256, 660, 256, 1, 2);
        value_row(0, tr("版本"), "", firmware_version(), 256, 70);
        value_row(0, tr("硬件"), "", hardware_name(), 326, 70);
        value_row(0, tr("作者"), "", "wulu", 396, 70);
        value_row(0, tr("项目主页"), "", kProjectHome, 466, 70);
        graphics::draw_qr(canvas, std::string("https://") + kProjectHome, 36, 572, 224);
        text(tr("扫码打开项目主页"), 284, 594, 28, 364, 1, true);
        wrapped(tr("查看介绍和字体下载。"), 284, 642, 364, 22, true, 3);
        text(tr("第三方组件"), 36, 840, 22, 612, 3);
        canvas.line(24, 876, 660, 876, 1, 2);
        value_row(0, tr("epdiy 显示驱动"), "", "LGPL-3.0-or-later", 876, 64);
        value_row(0, tr("内置正文字体"), "", "OFL 1.1", 940, 64);
        text(tr("GPL-3.0+ · 含 ESP-IDF 链接许可"), 36, 1036, 22, 612, 3);
        footer_bar(notice());
    }
    void help_regions(int x, int y, int width, int height) {
        const int left = width * 205 / 684, right = width * 479 / 684, foot = height * footer_top / 1216;
        const int device = width * reading_footer::device_left / 684;
        const std::array<Hit, 5> zones{{{x, y, left, foot, HelpPrevious},
            {x + left, y, right - left, foot, HelpTools}, {x + right, y, width - right, foot, HelpNext},
            {x, y + foot, device, height - foot, HelpPosition}, {x + device, y + foot, width - device, height - foot, HelpDevice}}};
        draw_turn_regions(x, y, width, foot, help_zone, 9);
        for (const auto& zone : zones) {
            const bool hidden = help_page == 3 && ((zone.id == HelpPosition && help_position == 4) ||
                (zone.id == HelpDevice && help_device == 2));
            if (zone.id >= HelpPosition)
                canvas.fill_rect(zone.x, zone.y, zone.w, zone.h, zone.id == help_zone && !hidden ? 1 : 15);
            hits.push_back(zone);
        }
        canvas.line(x, y + foot, x + width, y + foot, 9, 2);
        canvas.line(x + device, y + foot, x + device, y + height, 9, 2);
        outline(x, y, width, height);
    }
    void draw_help_demo();
    void draw_help() {
        view = View::Help;
        hits.clear(); canvas.clear(); header(tr("使用说明"), Back, help_from_settings ? tr("设置") : tr("书库"));
        const auto& page = kHelpPages[help_page];
        if (help_page == 3) { draw_help_demo(); return; }
        text(tr(page.title), 36, 144, 34, 612, 1, true);
        text("Pico Page · " + firmware_version(), 36, 204, 22, 612, 3);
        if (help_page == 2) { draw_help_demo(); return; }
        int body_height = 0;
        for (const auto& section : page.sections) body_height += wrapped(tr(section.body), 36, 0, 612, 26, false);
        const int bottom = help_page == 0 ? 980 : help_page == 1 ? 1034 : 1100;
        const bool compact_sections = 276 + body_height + static_cast<int>(page.sections.size()) * 84 - 36 > bottom;
        const int title_height = compact_sections ? 40 : 48, section_gap = compact_sections ? 12 : 36;
        int y = compact_sections ? 244 : 276;
        for (const auto& section : page.sections) {
            text(tr(section.title), 36, y, 28, 612, 1, true);
            y += title_height;
            y += wrapped(tr(section.body), 36, y, 612, 26);
            y += section_gap;
        }
        if (help_page == 0) {
            value_row(OnboardingReplay, tr("重新观看开机引导"), "", "", 1000, 82);
        }
        if (help_page == 1) text(tr("下一屏可直接点按试一试"), 36, 1058, 24, 612, 3);
        footer_bar("", help_page, kHelpPages.size());
    }
    std::vector<std::string> lock_screens;
    std::size_t lock_screen_page = 0;
    std::string lock_screen_error, custom_notice;
    std::string custom_rotation_name, custom_rotation_shown;
    std::chrono::steady_clock::time_point custom_retry_at{};
    std::optional<std::chrono::steady_clock::time_point> custom_sleep_deadline;
    std::unique_ptr<std::uint8_t, decltype(&std::free)> sleep_wait_footer{nullptr, &std::free};
    bool sleep_wait_restore_pending = false;
    View sleep_wait_view = View::Library;
    static constexpr unsigned kCustomPrepareAttempts = 3;
    unsigned custom_prepare_attempts = 0;
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
        loading(tr("正在读取休眠图片"));
    }
    void preview_lock_screen() {
#ifdef ESP_PLATFORM
        if (draft.sleep_overlay && (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 342U * 1216 + 768U * 1024 ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 342U * 1216)) {
            draw_power(tr("预览内存不足，请稍后重试。")); overlay(); return;
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
        loading(tr("正在准备图片预览"));
    }
    void draw_lock_screens() {
        view = View::LockScreens;
        hits.clear(); canvas.clear(); header(tr("自定义休眠图片"), Back, tr("休眠"));
        const bool pending = draft.sleep_image != edit_snapshot.config.settings.sleep_image ||
                             draft.sleep_screen != edit_snapshot.config.settings.sleep_screen;
        const char* state = pending ? tr("待保存") : tr("当前");
        file_choice(SleepRotation, tr("按文件名轮播"), tr("每次休眠使用下一张图片"), 120, draft.sleep_image.empty(),
                    draft.sleep_image.empty() ? state : nullptr);
        text("LockScreens/ · PNG / JPEG", 36, 262, 24, 612, 3);
        const auto pages = std::max<std::size_t>(1, (lock_screens.size() + 5) / 6);
        lock_screen_page = std::min(lock_screen_page, pages - 1);
        for (std::size_t i = lock_screen_page * 6; i < lock_screens.size() && i < (lock_screen_page + 1) * 6; ++i) {
            const bool selected = draft.sleep_image == lock_screens[i];
            file_choice(LockScreenBase + static_cast<int>(i), lock_screens[i], "", 300 + static_cast<int>(i % 6) * 90,
                        selected, selected ? state : nullptr);
        }
        if (lock_screens.empty()) note(lock_screen_error.empty() ? tr("请将图片放入 TF 卡 LockScreens/ 目录。") : lock_screen_error, 380);
        button(SleepRefresh, tr("重新读取"), 36, 980, 280, 88);
        button(Back, tr("完成"), 340, 980, 308, 88, true);
        footer_bar(tr("返回后保存设置"), lock_screen_page, pages);
    }
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
        if ((view == View::Sync && sync_subtab == SyncSubtab::Clock) ||
            (view == View::WifiSettings && wifi_kind == WifiOperation::Kind::Clock)) {
            sleep_wait_footer.reset();
            sleep_wait_restore_pending = false;
            if (view == View::Sync) draw_sync();
            else draw_wifi();
            overlay();
            return;
        }
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
        text(tr("正在准备休眠，按电源键取消"), 36, footer_top + 28, 24, 612, 1);
        present_sleep_wait_footer();
    }
    bool custom_prepare_due() const {
        return settings.sleep_screen == "custom" && !custom_prepare_finished &&
               custom_prepare_attempts < kCustomPrepareAttempts &&
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
    std::string typography_summary(const ReaderSettings &values) const {
        return font_name(values.font) + tr(" · 字号 ") + std::to_string(values.font_size_px) + tr(" · 行距 ") + compact(values.line_height);
    }
    const char *hardware_name() const {
#ifdef ESP_PLATFORM
        return "Read Pico";
#else
        return tr("桌面预览");
#endif
    }
    static std::string firmware_version() {
#ifdef ESP_PLATFORM
        return esp_app_get_description()->version;
#else
        return PICO_PAGE_VERSION;
#endif
    }
    StorageUsage storage_usage;
    void begin_storage(bool clear_cache = false);
    void draw_storage();
    void draw_input_settings() {
        if (!bookmark_io_ready()) return;
        view = View::InputSettings;
        hits.clear(); canvas.clear(); header(tr("书库"), Back, tr("设置"));
        const auto values = store.load_settings().settings;
        value_row(LibraryView, tr("书库默认视图"), tr("点按切换"), values.library_view == "grid" ? tr("卡片") : tr("列表"), 104, 104, false, false);
        value_row(LibraryFolders, tr("书库文件夹"), tr("最近阅读排序时生效"), values.library_folders == "first" ? tr("置顶") : tr("跟随最近"), 208, 104, false, false);
        footer_bar(notice());
    }
    void open_account() {
        sync::Config saved;
        std::string detail;
        std::fill(account.password.begin(), account.password.end(), '\0');
        account = {};
        account.had_saved_config = sync::load_config(root, saved, detail);
        account.endpoint = account.had_saved_config ? saved.endpoint : provision::default_endpoint;
        account.username = saved.username;
        account_page = AccountPage::Form;
        account_notice.clear();
    }
    bool account_config(sync::Config& next) const {
        sync::Config saved;
        std::string detail;
        const bool had = sync::load_config(root, saved, detail);
        next = had ? saved : sync::Config{};
        next.endpoint = account.endpoint;
        next.username = account.username;
        if (!account.password.empty()) next.userkey = md5_hex(account.password);
        else if (!had || saved.username != account.username) next.userkey.clear();
        if (next.device_id.empty()) next.device_id = writer;
        return sync::valid_config(next);
    }
    void draw_sync_settings();
    const char* account_kept() const {
        return !account.had_saved_config ? tr("账户没有保存。") : account.changed ? tr("修改没有保存，原来的账户保留。") : tr("原来的账户保留。");
    }
    void verify_account();
    void cancel_account() {
        if (!account_check) return;
        network::release(account_hold);
        if (network_cancel) network_cancel->store(true);
        active_network = 0;
        network_request.reset();
        account_check.reset();
        account_page = AccountPage::Form;
    }
    void account_complete(int status, const std::string& failure, LinkFailure link) {
        const auto checked = *std::exchange(account_check, std::nullopt);
        network::release(account_hold);
        active_network = 0;
        account_page = AccountPage::Form;
        std::string detail;
        if (!failure.empty()) account_notice = trf("{failure}{account}", {{"failure", link != LinkFailure::None ? failure : tr("没有连上同步服务器。")}, {"account", account_kept()}});
        else if (status == 401 || status == 403) {
            account.check = AccountDraft::Check::Rejected;
            account_notice = account.had_saved_config && !account.changed ? tr("服务器拒绝了已保存的账户，账户或密码不正确。")
                                                               : trf("账户或密码不正确。{account}", {{"account", account_kept()}});
        }
        else if (status != 200) account_notice = trf("同步服务返回 HTTP {status}，无法确认账户。{account}", {{"status", std::to_string(status)}, {"account", account_kept()}});
        else if (store.save_private("sync.toml", provision::sync_toml(checked), detail) != StoreStatus::Ok)
            account_notice = tr("账户已通过验证，但没有写入 TF 卡。请检查 TF 卡。");
        else {
            open_account();
            account.check = AccountDraft::Check::Accepted;
            refresh_badge_config();
            account_notice = tr("账户已验证并保存。");
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
    void confirm_page(const std::string &origin, const std::string &question, const std::string &object,
                      const std::string &path, const std::string &consequence) {
        hits.clear(); canvas.clear(); header(origin, 0);
        text(question, 36, 160, 32, 612, 1, true);
        int y = 244;
        if (!object.empty()) {
            title_lines(library_text_font(), object, 36, y, 612, 30);
            y += std::min(wrapped(library_text_font(), object, 0, 0, 612, 30, false), 80) + 4;
            title_lines(library_text_font(), path, 36, y, 612, 22, false, 2, 3);
            y += std::min(wrapped(library_text_font(), path, 0, 0, 612, 22, false), 64) + 28;
        }
        wrapped(consequence, 36, y, 612, 28);
    }
    void confirm_management(bool remove) {
        if (!persist()) { error(message); return; }
        EpubDocument inspected;
        std::string detail;
        if (!remove && (!inspected.open(root + "/" + detail_path, detail) ||
                        inspected.metadata().book_id != detail_metadata.book_id)) {
            error(tr("原书已变化，请重新打开详情。")); return;
        }
        if (store.book_snapshot(detail_path, management_snapshot) != StoreStatus::Ok) {
            error(tr("无法读取文件状态，请检查 TF 卡后重新打开详情。")); return;
        }
        delete_original = remove;
        view = View::BookManagement;
        confirm_page(tr("书籍管理"), remove ? tr("删除这本书？") : tr("清除本地进度？"),
                     detail_metadata.title.empty() ? basename(detail_path) : detail_metadata.title, detail_path,
                     remove ? tr("将删除 TF 卡中的原书文件。阅读记录和排版设置会保留；把同一本书放回原文件夹并使用原文件名，即可恢复。服务器上的记录不受影响。")
                            : tr("将清除本书在本机的阅读位置及其备份，下次打开从头开始。原书和排版设置会保留。服务器上的记录不受影响。"));
        button(Back, tr("取消"), 24, 1080, 306, 96, true);
        danger_button(ManageConfirm, remove ? tr("删除原书") : tr("清除进度"), 354, 1080, 306);
    }
    void manage_book();
    void cover_title(const std::string &title, int x, int top, int width, int height, int initial_size) {
        const auto content = decode_utf8(title);
        graphics::LayoutSpec spec;
        spec.left = x; spec.top = top; spec.width = width; spec.height = height;
        spec.indent_chars = 0;
        graphics::Page title_page;
        for (int size = initial_size; size >= 28; size -= 2) {
            body.set_pixel_size(size);
            spec.font_px = size;
            spec.line_height = std::max(size * 5 / 4, body.metrics().ascender + body.metrics().descender);
            title_page = graphics::layout_page(body, content, 0, spec);
            if (title_page.end == content.size()) break;
        }
        graphics::render_page(canvas, body, title_page);
    }
    void draw_sleep(const std::string &sync_state);
    void entry_row(int id, const std::string &title, const std::string &subtitle, const std::string &value,
                   int y, int height) {
        const auto shown = value.empty() ? std::string("›") : value + " ›";
        const int value_width = std::min(text_width(shown, 24), 300);
        const int title_width = 612 - value_width - 16;
        if (subtitle.empty())
            text(title, 36, top_for_middle(ui, 28, y + height / 2), 28, title_width);
        else {
            text(title, 36, y + height / 2 - 46, 28, title_width);
            text(subtitle, 36, y + height / 2 + 6, 24, title_width, 3);
        }
        text(shown, 648 - value_width, top_for_middle(ui, 24, y + height / 2), 24, value_width, 3);
        canvas.line(36, y + height - 1, 647, y + height - 1, 12, 2);
        hits.push_back({24, y, 636, height, id});
    }
    void draw_import();
    int upload_summary(int y, int title_size) {
        const bool any = portal.saved_books > 0;
        text(portal_closing ? tr("正在结束…") : portal.phase == PortalPhase::Receiving ? tr("正在接收 / 保存…") : any ? trf("已保存 {count} 个文件", {{"count", std::to_string(portal.saved_books)}}) : tr("等待手机上传"),
             36, y, title_size, 612, 1, true);
        if (!portal_closing) {
            if (any) {
                text(tr("最近：") + portal.latest_book, 36, y + title_size + 22, 24, 612, 3);
                return y + title_size + 56 + wrapped(tr("逐个结果见浏览器。"), 36, y + title_size + 56, 612, 24, true, 3);
            } else
                return y + title_size + 22 + wrapped(tr("逐个结果见浏览器。结束后书库和字体列表会更新。"), 36, y + title_size + 22, 612, 24, true, 3);
        }
        return y + title_size;
    }
    int portal_summary_line = 0;
    void draw_portal();
    void draw_usb() {
        view = View::UsbTransfer;
        hits.clear();
        canvas.clear();
        if (usb_confirm) {
            confirm_page(tr("USB 文件传输中"), tr("退出 USB 传输？"), "", "",
                         tr("请先在电脑上安全弹出设备。确认后设备会重启，并回到书库。"));
            button(UsbCancel, tr("取消"), 24, 1080, 306, 96);
            button(UsbExitConfirm, tr("退出并重启"), 354, 1080, 306, 96, true);
        } else {
            header(tr("USB 文件传输"), 0);
            text(usb_active ? tr("USB 文件传输已开启") : tr("USB 文件传输未开启"), 36, 160, 40, 612, 1, true);
            const int height = wrapped(usb_active ? tr("用数据线连接电脑后，TF 卡会显示为可移动磁盘。传输期间设备暂停阅读和同步。完成后先在电脑上安全弹出，再退出并重启。")
                                                  : tr("传输期间设备暂停阅读和同步。完成后先在电脑上安全弹出，再退出并重启。"), 36, 244, 612, 28);
            if (!message.empty()) wrapped(message, 36, 272 + height, 612, 26);
            button(UsbExit, tr("退出并重启"), 24, 1080, 636, 96);
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
        remote_fraction = -1;
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
    bool remote_chapter(std::size_t& index) const {
        const auto& spine = document.spine();
        return remote_anchor && remote_place && document.chapter_index_for(remote_anchor->resource, index) &&
               spine[index].linear && chapter_index < spine.size() && spine[chapter_index].linear;
    }
    std::string remote_book_percent() const {
        std::size_t index;
        if (!remote_chapter(index)) return {};
        return std::to_string(book_percent_value(index, *remote_place, remote_place->offset)) + "%";
    }
    std::string remote_position() const {
        if (remote_anchor) {
            const auto percent = remote_book_percent();
            return anchor_label(*remote_anchor) + (percent.empty() ? "" : " · " + percent);
        }
        return remote_fraction >= 0 ? tr("约 ") + percent_text(remote_fraction) : tr("位置未知");
    }
    int further() const {
        std::size_t index;
        if (!remote_chapter(index)) return 0;
        if (index != chapter_index) return index > chapter_index ? 1 : -1;
        if (remote_place->part != chapter.part) return remote_place->part > chapter.part ? 1 : -1;
        return remote_place->offset >= page.end ? 1 : remote_place->offset < page.start ? -1 : 0;
    }
    void sync_cancelled(bool sent) {
        cancel_sync();
        sync_status = sent ? tr("已取消。已发出的上传仍可能生效，请重新检查远端。") : tr("已取消。本机位置没有改动。");
    }
    void draw_network_closing() {
        const bool account_page_waiting = network_return_view == View::SyncSettings;
        view = View::NetworkClosing;
        hits.clear(); canvas.clear(); header(account_page_waiting ? tr("KOReader 同步") : tr("同步"), 0);
        note(account_page_waiting ? tr("正在结束验证…") : tr("正在结束同步…"), 246);
        wrapped(account_page_waiting ? trf("网络任务正在结束。{account}", {{"account", account_kept()}})
                : network_closing_upload ? tr("网络任务正在结束。已发出的上传仍可能生效，下次同步需重新检查远端。")
                : tr("网络任务正在结束。本机位置没有改动。"), 36, 336, 612, 26);
        button(ContinueOffline, return_label(origin_for(view)), 24, 1080, 636, 96);
    }
    void wait_network(int next_control) {
        network_after_cancel = next_control;
        network_return_view = view;
        network_closing_upload = uploading;
        sync_cancelled(uploading);
        draw_network_closing(); overlay();
    }
    struct SyncAction { int id; const char* label; bool primary = false; };
    void sync_actions(SyncAction left, SyncAction right);
    void sync_information_card(const std::string& label, const std::string& value, const std::string& detail,
                               int x, int width, graphics::Font& value_font, int value_size = 28);
    void sync_hint(const std::string& hint, std::uint8_t gray = 3);
    void draw_sync_progress(bool enabled, bool saved_network, const std::string& network_problem);
    void draw_sync_clock(std::time_t now, bool enabled, bool saved_network, const std::string& network_problem,
                         const std::string& network_name);
    void draw_sync_comparison();
    void draw_sync();
    void check_sync(bool automatic = false);
    void confirm_upload() {
        const bool replaces = sync_state == SyncState::Different || sync_state == SyncState::Unmapped;
        const auto local = local_position();
        const auto remote = remote_position();
        const auto device = remote_device.empty() ? std::string(tr("其他设备")) : remote_device + " ";
        if (!panel_background(752, false)) return;
        view = View::Confirmation;
        canvas.line(0, 752, 683, 752, 1, 3);
        if (replaces) {
            text(tr("用本机位置替换远端？"), 36, 786, 32, 612, 1, true);
            text(tr("本机　") + local, 36, 852, 26, 612);
            text(tr("远端　") + remote + (remote_device.empty() ? "" : "（" + remote_device + "）"), 36, 896, 26, 612);
            wrapped(trf("替换后，{device}下次同步会回到 {position}。", {{"device", device}, {"position", book_percent()}}), 36, 952, 612, 24, true, 3);
            button(Back, tr("取消"), 24, 1080, 306, 96, true);
            button(SyncConfirm, tr("替换"), 354, 1080, 306, 96);
        } else {
            text(tr("上传本机位置？"), 36, 786, 32, 612, 1, true);
            wrapped(trf("{position} 将保存到服务器。其他设备下次同步会收到这个位置。", {{"position", local}}), 36, 852, 612, 26);
            button(Back, tr("取消"), 24, 1080, 306, 96);
            button(SyncConfirm, tr("上传"), 354, 1080, 306, 96, true);
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
            if (memory_error(detail)) sync_status = tr("设备内存不足，暂时无法定位远端位置，没有跳转。");
            else if (detail == sync::kLongSourceNode)
                sync_status = tr("这一章含有超长文本节点，暂时无法准确换算位置，没有跳转。");
            else if (outside_mapping(detail))
                sync_status = tr("远端位置所在的章节含有表格等暂不支持的内容，无法换算成本机位置，没有跳转。");
        }
    }
    static bool outside_mapping(const std::string& detail) {
        return detail == sync::kUnsupportedStructure || detail == sync::kUnresolvedVisibility || detail == sync::kLongSourceNode;
    }
    static void log_mapping_failure([[maybe_unused]] const char* direction, [[maybe_unused]] const std::string& detail) {
#ifdef ESP_PLATFORM
        ESP_LOGW("reader_sync", "%s mapping failed: %s", direction, detail.c_str());
#endif
    }
    void upload_mapping_failed(const std::string& detail) {
        log_mapping_failure("Upload", detail);
        sync_status = memory_error(detail) ? tr("设备内存不足，暂时无法计算上传位置。") :
            detail == sync::kLongSourceNode ? tr("这一章含有超长文本节点，暂时无法准确换算位置，没有上传。") :
            outside_mapping(detail) ? tr("这一章含有表格等暂不支持的内容，无法换算成 KOReader 的位置，没有上传。") :
            tr("这个位置暂时不能上传。");
    }
    void upload();
    void send_upload(const ReadingRecord& record, const std::string& xpointer, double progress_fraction);
    void network_complete(std::uint64_t id, int status, const std::string &bytes,
                          const std::string &failure, LinkFailure link);
    std::string reason(const std::string &detail, const char *fallback = "操作失败") const {
        if (auto message = font_error_message(detail, settings.language); !message.empty()) {
#ifdef ESP_PLATFORM
            ESP_LOGW("reader_app", "Font failure: %s", detail.c_str());
#endif
            return message;
        }
        std::string lower = detail;
        for (auto &c : lower) if (c >= 'A' && c <= 'Z') c += 32;
        const auto has = [&](const char *word) { return lower.find(word) != std::string::npos; };
        if (lower == "missing epub mimetype") return tr("EPUB 缺少格式标识");
        if (lower == "epub mimetype exceeds limit") return tr("EPUB 格式标识过长");
        if (lower == "cannot read epub mimetype") return tr("EPUB 格式标识读取失败");
        if (lower == "invalid epub mimetype") return tr("EPUB 格式标识不正确");
        if (has("memory") || has("alloc")) return tr("设备内存不足");
        if (has("changed")) return tr("文件在读取过程中发生了变化");
        if (has("unsupported") || has("requires") || has("exactly") || has("not retained")) return tr("内容结构暂不支持");
        if (has("unavailable") || has("no longer") || has("cannot be read") || has("read failed") ||
            has("cannot open") || has("cannot seek") || has("cannot hash") || has("cannot read"))
            return tr("文件无法读取，请检查 TF 卡");
        if (detail.find(": ") == std::string::npos &&
            std::any_of(detail.begin(), detail.end(), [](unsigned char c) { return c >= 0x80; })) {
            const std::string stop = "。";
            const bool ended = detail.size() >= stop.size() && detail.compare(detail.size() - stop.size(), stop.size(), stop) == 0;
            std::string translated = tr(detail.c_str());
            if (ended) {
                if (translated.size() >= stop.size() && translated.compare(translated.size() - stop.size(), stop.size(), stop) == 0)
                    translated.resize(translated.size() - stop.size());
                else if (!translated.empty() && translated.back() == '.') translated.pop_back();
            }
            return translated;
        }
        return tr(fallback);
    }
    std::string retry_path;
    void error(const std::string &text_in, const std::string &retry = "", bool repeatable = true);
    void open_failed(const std::string& detail, const std::string& path) {
        const auto cause = reason(detail, tr("无法解析文件内容"));
        const bool transient = cause == tr("设备内存不足") || cause == tr("文件在读取过程中发生了变化") ||
                               cause == tr("文件无法读取，请检查 TF 卡") || cause == tr("EPUB 格式标识读取失败");
        error(cause + tr("。原书没有被修改。"), path, transient);
    }
    std::string settings_file_problem(const std::vector<ConfigDiagnostic>& diagnostics) const {
        for (const auto& diagnostic : diagnostics)
            if (diagnostic.status == StoreStatus::IoError) return tr("设置文件无法读取");
        for (const auto& diagnostic : diagnostics)
            if (diagnostic.status == StoreStatus::UnsupportedVersion) return tr("设置文件来自较新的固件");
        return tr("设置文件内容有误");
    }
    void back();
    bool begin_settings_edit(bool global = false) {
        if (!bookmark_io_ready()) return false;
        auto snapshot = store.settings_snapshot(global ? "" : book_path);
        if (!snapshot.config.diagnostics.empty()) {
            error(settings_file_problem(snapshot.config.diagnostics) + tr("。原文件未修改，请在电脑上检查后重新打开设置。"));
            return false;
        }
        edit_snapshot = std::move(snapshot);
        return true;
    }
    void wifi_control(int id);
    void control(int id, PageTurnOrigin turn_origin = PageTurnOrigin::Middle);
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

}
