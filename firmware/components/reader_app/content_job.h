#pragma once
#include "pico_page/document.h"
#include "pico_page/dictionary_selection.h"
#include "pico_page/dictionary_text.h"
#include "pico_page/diagnostics.h"
#include "page_cache.h"
#include "content_cache.h"
#include "statistics_work.h"
#include "pico_page/graphics.h"
#include "pico_page/store.h"
#include "pico_page/sync.h"
#include <sys/types.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#ifndef ESP_PLATFORM
#include <thread>
#endif

namespace pico_page {
inline constexpr std::size_t kCoverFailureBytes = 71;
std::string cover_failure_payload(const std::string& identity, ContentFailureKind kind);
ContentFailureKind cached_cover_failure(std::string_view payload, const std::string& identity);

inline constexpr char kTxtUtf8Revision[] = "txt-utf8-v1";
inline constexpr char kTxtGb18030Revision[] = "txt-gb18030-v1";
inline constexpr char kTxtUtf16LeRevision[] = "txt-utf16le-v1";
inline constexpr char kTxtUtf16BeRevision[] = "txt-utf16be-v1";
inline const char* txt_content_revision(TxtEncoding encoding) {
    switch (encoding) {
    case TxtEncoding::Utf8: break;
    case TxtEncoding::Gb18030: return kTxtGb18030Revision;
    case TxtEncoding::Utf16Le: return kTxtUtf16LeRevision;
    case TxtEncoding::Utf16Be: return kTxtUtf16BeRevision;
    }
    return kTxtUtf8Revision;
}
inline constexpr int kLibraryCoverWidth = 184, kLibraryCoverHeight = 250;
inline constexpr auto kLockScreenPrepareBudget = std::chrono::seconds(20);
std::size_t readable_offset(const Chapter& source, std::size_t start, bool author = false);
std::size_t restore_readable_offset(const Chapter& source, std::size_t start);
const DocumentImage* image_at(const Chapter& source, std::size_t start);
graphics::Page plan_content(graphics::Font& font, const Chapter& source, std::size_t start,
                            const graphics::LayoutSpec& spec, bool collect,
                            const std::function<bool()>& stop = {}, const graphics::AuthorFonts* fonts = nullptr);
graphics::AuthorFonts prepare_embedded_fonts(const FileStore& store, EpubDocument& document, const Chapter& chapter,
    bool& stable, const std::function<bool()>& stop = {});

std::string image_cache_key(const std::string& book_id, const std::string& resource, const graphics::LayoutSpec& spec);
std::string page_image_cache_key(const std::string& book_id, const graphics::Page& page, const graphics::LayoutSpec& spec);

bool read_body_image_cache(CacheReader& reader, graphics::Canvas& image, const std::function<bool()>& stop);
bool write_body_image_cache(const FileStore& store, const std::string& name, const graphics::Canvas& image,
                            const std::function<bool()>& stop);

bool read_lock_screen_cache(CacheReader& reader, const std::string& header, graphics::Canvas* image,
                            bool& alpha, const std::function<bool()>& stop);

struct PaginationSession {
    EpubDocument document;
    graphics::Font font;
    Chapter chapter;
    graphics::AuthorFonts author_fonts;
    std::string path, font_path;
    std::size_t chapter_index = 0;
    std::uint32_t part = 0;
    bool opened = false, font_opened = false, loaded = false;
};

struct ChapterSession {
    EpubDocument document;
    std::string path;
};

struct DictionarySession {
    struct Source {
        std::string relative_path, path;
        dictionary::Verification verification;
    };
    std::vector<Source> verified;
    std::vector<std::pair<std::string, dictionary::Result>> preparation_failures;
};
struct DictionaryItem {
    std::string id;
    dictionary::Info info;
    dictionary::Result status;
};

class ContentJob {
public:
    enum class Kind { Pagination, FontIdentity, Image, ImageViewer, Open, Chapter, SaveProgress, LibraryCovers, PageCacheRead, PageCacheWrite, Inspect, SleepCover, MapUpload, MapDownload, SaveBookmarks, LockScreenList, LockScreen, ContentCacheWrite, LockScreenPreview, DictionaryList, DictionaryPrepare, DictionaryLookup, DictionaryRead, DictionaryPage, StorageScan, ClearCache, ReadingStatistics };
    static bool dictionary_kind(Kind kind) {
        return kind == Kind::DictionaryList || kind == Kind::DictionaryPrepare ||
               kind == Kind::DictionaryLookup || kind == Kind::DictionaryRead || kind == Kind::DictionaryPage;
    }
    enum class Target { Start, End, Before, Anchor, Fragment };
    enum class StartFailure { None, InternalMemory, TaskCreate };
    static constexpr std::size_t cold_image_memory_required_bytes = 2 * (684U * 1216U / 2) + 768U * 1024;
    struct CoverSource {
        std::string path, cache_name, cache_header;
        off_t size = 0;
        time_t modified = 0;
    };
    struct Request {
        std::string path, font;
        std::string sleep_image;
        std::shared_ptr<StatisticsWork> statistics;
        bool sleep_overlay = false;
        std::shared_ptr<graphics::Canvas> lock_background;
        std::string lock_screen_shown;
        Kind kind = Kind::Pagination;
        Target target = Target::Start;
        Anchor anchor;
        std::string fragment, xpointer;
        int dom_version = 0;
        std::optional<std::size_t> known_start;
        std::optional<std::size_t> known_end;
        std::string chapter_resource;
        std::string root, image_resource, cache_name, relative_path, content_revision;
        DocumentMetadata identity;
        EpubDocument::VerifiedIndex book_index;
        graphics::LayoutSpec layout;
        graphics::Page page;
        std::vector<CoverSource> covers;
        std::string cache_header;
        std::optional<PageCache> page_cache;
        std::shared_ptr<const ContentCacheEntry> content_cache;
        bool content_cache_write_needed = false;
        std::vector<bool> page_linear;
        std::optional<ReadingRecord> record;
        std::shared_ptr<FileDigest> font_scan;
        std::optional<BookmarkList> bookmark_base;
        std::vector<Bookmark> bookmarks;
        std::uint64_t bookmark_next = 1;
        std::shared_ptr<PaginationSession> session;
        std::shared_ptr<ChapterSession> chapter_session;
        std::shared_ptr<DictionarySession> dictionary_session;
        std::vector<std::string> dictionary_sources;
        std::vector<dictionary::Candidate> dictionary_candidates;
        int dictionary_navigation = 0;
        std::uint32_t dictionary_ordinal = 0;
        std::shared_ptr<const std::vector<dictionary::Field>> dictionary_definition;
        dictionary::TextCursor dictionary_cursor;
        std::uint32_t dictionary_seek_pages = 0;
        bool dictionary_replay = false;
        bool dictionary_reflow = false, dictionary_full_layout = false;
        std::size_t dictionary_layout_anchor = 0;
        bool dictionary_entry_lookup = false;
        bool dictionary_retry = false;
        bool save_last_book = false, cover_crop = false, preview = false;
        bool lock_screen_prepare = false;
#ifndef ESP_PLATFORM
        std::size_t lock_cache_limit_for_test = 0;
        std::function<void()> lock_cache_validated_for_test;
        std::size_t lock_canvas_limit_for_test = 0;
#endif
        std::chrono::steady_clock::time_point lock_screen_deadline{};
        bool progressive = false;
        bool chapter_prefetch = false;
        std::uint64_t generation = 0;
        PP_DIAGNOSTIC(std::uint64_t diagnostic_session = 0, diagnostic_layout = 0;)
        std::size_t chapter = 0, offset = 0;
        std::uint32_t part = 0;
    };
    struct Result {
        graphics::AuthorFonts author_fonts;
        bool author_fonts_stable = true;
        StorageUsage storage;
        StoreStatus clear_cache_status = StoreStatus::IoError;
        std::vector<std::uint32_t> starts;
        std::size_t offset = 0;
        bool part_done = false, last_part = false;
        std::string error, font_identity, xpointer;
        std::vector<std::string> lock_screens;
        std::vector<DictionaryItem> dictionaries;
        std::vector<DictionaryItem> dictionary_failed_sources;
        dictionary::Result dictionary_status;
        dictionary::Info dictionary_info;
        std::optional<dictionary::Candidate> dictionary_match;
        std::vector<dictionary::Entry> dictionary_entries;
        std::vector<dictionary::Field> dictionary_fields;
        bool dictionary_complete = false, dictionary_more = false, dictionary_reused = false;
        std::size_t dictionary_source = 0;
        bool dictionary_partial = false;
        bool dictionary_missing_glyphs = false;
        graphics::Page dictionary_page;
        dictionary::TextCursor dictionary_next;
        dictionary::TextCursor dictionary_current;
        std::uint32_t dictionary_page_number = 0;
        std::string lock_screen;
        Anchor mapped_anchor;
        sync::PartPosition mapped_place;
        double progress_fraction = 0;
        std::unique_ptr<graphics::Canvas> image;
        std::vector<std::size_t> failed_images;
        PP_DIAGNOSTIC(std::uint32_t decoded_images = 0;)
        std::vector<std::optional<std::vector<std::uint8_t>>> covers;
        std::unique_ptr<EpubDocument> document;
        Chapter chapter;
        ContentCacheEntry content_cache;
        bool retained_cache_hit = false;
        std::size_t chapter_stream_bytes = 0;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> history;
        std::size_t chapter_index = 0;
        ConfigResult configuration;
        ProgressResult progress;
        StoreStatus bookmark_status = StoreStatus::IoError;
        BookmarkList bookmarks;
        bool last_book_saved = false;
        bool cache_finished = false;
        ContentFailureKind failure_kind = ContentFailureKind::None;
        bool retryable_failure() const { return content_failure_is_retryable(failure_kind); }
        bool cursor_saved = false;
        std::optional<PageCache> page_cache;
        off_t file_size = 0;
        time_t file_modified = 0;
        ino_t file_inode = 0;
    };
    explicit ContentJob(Request request);
    ~ContentJob();
    ContentJob(const ContentJob&) = delete;
    ContentJob& operator=(const ContentJob&) = delete;
#ifdef ESP_PLATFORM
    static StartFailure prepare_worker();
#endif
    bool start();
    StartFailure start_failure() const { return start_failure_; }
#ifndef ESP_PLATFORM
    static void fail_starts_for_test(Kind kind, unsigned count, StartFailure failure);
    static void delay_starts_for_test(Kind kind, std::chrono::milliseconds delay);
#endif
    bool started() const { return started_; }
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    bool pause() {
        auto generation = pause_generation_.load(std::memory_order_relaxed);
        if (!(generation & 1U)) {
            ++generation;
            pause_generation_.store(generation, std::memory_order_release);
        }
        return paused_generation_.load(std::memory_order_acquire) == generation;
    }
    void resume() {
        const auto generation = pause_generation_.load(std::memory_order_relaxed);
        if (generation & 1U) pause_generation_.store(generation + 1, std::memory_order_release);
    }
    bool done() const { return done_.load(std::memory_order_acquire); }
    void wait();
    const Request request;
    Result result;
private:
    bool checkpoint();
    void run();
#ifdef CONFIG_PICO_PAGE_PERF_METRICS
    void execute();
    diagnostics::Token measurement_{};
#endif
    void render_image();
    void read_dictionary();
    dictionary::Result open_dictionary(FileStore& store, const std::string& relative_path,
                                       dictionary::Dictionary& reader, const dictionary::Stop& stop);
    void search_dictionaries(FileStore& store, const dictionary::Stop& stop);
    void prepare_lock_screen();
    void open_book();
    void prepare_cover();
    void map_position();
    void load_chapter();
    void library_covers();
    std::atomic<bool> cancelled_{false}, done_{false};
    std::atomic<std::uint32_t> pause_generation_{0}, paused_generation_{0};
    bool started_ = false;
    StartFailure start_failure_ = StartFailure::None;
#ifndef ESP_PLATFORM
    std::thread thread_;
#endif
};
}
