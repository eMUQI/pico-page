#pragma once
#include "pico_page/document.h"
#include "pico_page/dictionary_selection.h"
#include "pico_page/dictionary_text.h"
#include "pico_page/diagnostics.h"
#include "page_cache.h"
#include "content_cache.h"
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
/** Negative cover payload binds a definitive failure to SHA-256, independent of path timestamps. */
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
// Library card thumbnails are cropped to fill this frame; the size is part of the cache entry name.
inline constexpr int kLibraryCoverWidth = 184, kLibraryCoverHeight = 250;
inline constexpr auto kLockScreenPrepareBudget = std::chrono::seconds(20);
std::size_t readable_offset(const Chapter& source, std::size_t start);
const DocumentImage* image_at(const Chapter& source, std::size_t start);
graphics::Page plan_content(graphics::Font& font, const Chapter& source, std::size_t start,
                            const graphics::LayoutSpec& spec, bool collect,
                            const std::function<bool()>& stop = {});

std::string image_cache_key(const std::string& book_id, const std::string& resource, const graphics::LayoutSpec& spec);

/** Reads gray-v1 pixels directly into an unpublished 684 x 1216 canvas and verifies the digest.
 * Returns false on invalid data, I/O failure or cancellation; the caller must discard the canvas.
 * The single-owner reader must be positioned at byte zero.
 */
bool read_body_image_cache(CacheReader& reader, graphics::Canvas& image, const std::function<bool()>& stop);
/** Writes gray-v1 pixels without copying the canvas. False leaves the prepared image usable.
 * Checks cancellation between bounded writes; the final synchronized publication is not interruptible.
 * The caller owns the canvas and storage until this call returns.
 */
bool write_body_image_cache(const FileStore& store, const std::string& name, const graphics::Canvas& image,
                            const std::function<bool()>& stop);

/** Reads and checks one PPLOCK4 cache using a single row buffer. A null image only validates.
 * A supplied canvas must be unpublished and 684 x 1216; discard it on failure or cancellation.
 * Each call rewinds the same reader and validates header, length and payload digest.
 */
bool read_lock_screen_cache(CacheReader& reader, const std::string& header, graphics::Canvas* image,
                            bool& alpha, const std::function<bool()>& stop);

/**
 * Open book, body font and loaded chapter part shared by the pagination jobs of one counting
 * session, so that a job does not repeat the preparation of the previous one.
 * Only the job that received the session accesses its members until that job is done. The
 * owner may drop its reference at any time; the session then ends with the running job.
 */
struct PaginationSession {
    EpubDocument document;
    graphics::Font font;
    Chapter chapter;
    std::string path, font_path;
    std::size_t chapter_index = 0;
    std::uint32_t part = 0;
    bool opened = false, font_opened = false, loaded = false;
};

/** One optional sequential reader, accessed exclusively by its running chapter job.
 * The app may drop its reference at any time; the job retains it until completion.
 * End the session before changing books, handing off storage or preparing sleep.
 */
struct ChapterSession {
    EpubDocument document;
    std::string path;
};

/** Mount-scoped verification state. Only its currently running content job may
 * access it. Reset after USB, remount or rescan, once the old job has finished.
 * No dictionary files remain open between jobs.
 */
struct DictionarySession {
    /** path is the checked absolute .ifo path, kept only after a successful open. */
    struct Source {
        std::string relative_path, path;
        dictionary::Verification verification;
    };
    std::vector<Source> verified;
    /** At most 32 definitive InvalidFormat/Unsupported preparation results, never entry,
     * I/O, memory or cancellation failures. Explicit retry and preparation clear an entry.
     */
    std::vector<std::pair<std::string, dictionary::Result>> preparation_failures;
};
struct DictionaryItem {
    std::string id;
    dictionary::Info info;
    dictionary::Result status;
};

/** A single job owns its parser and font, or borrows them from a PaginationSession. Only done() permits access to its result. */
class ContentJob {
public:
    enum class Kind { Pagination, FontIdentity, Image, ImageViewer, Open, Chapter, SaveProgress, LibraryCovers, PageCacheRead, PageCacheWrite, Inspect, SleepCover, MapUpload, MapDownload, SaveBookmarks, LockScreenList, LockScreen, ContentCacheWrite, LockScreenPreview, DictionaryList, DictionaryPrepare, DictionaryLookup, DictionaryRead, DictionaryPage, StorageScan, ClearCache };
    static bool dictionary_kind(Kind kind) {
        return kind == Kind::DictionaryList || kind == Kind::DictionaryPrepare ||
               kind == Kind::DictionaryLookup || kind == Kind::DictionaryRead || kind == Kind::DictionaryPage;
    }
    enum class Target { Start, End, Before, Anchor, Fragment };
    enum class StartFailure { None, InternalMemory, TaskCreate };
    /**
     * Free PSRAM a cold image decode requires before reading the image: the result canvas, a raster of
     * at most the same size, and the 768 KiB kept free while a document is read. The encoded bytes are
     * admitted separately, by their actual size, in EpubDocument::read_image.
     */
    static constexpr std::size_t image_room = 2 * (684U * 1216U / 2) + 768U * 1024;
    struct CoverSource {
        std::string path, cache_name, cache_header;
        off_t size = 0;
        time_t modified = 0;
    };
    struct Request {
        std::string path, font;
        std::string sleep_image;
        bool sleep_overlay = false;
        /** Owned output initialized with the pre-sleep/preview frame. After enqueue only the worker
         * may access it; no references to the application's live or in-flight canvas are retained. */
        std::shared_ptr<graphics::Canvas> lock_background;
        /** LockScreen: the rotation image last presented but not yet recorded. The job records it
         * before choosing the next image; a restart before any such job repeats that image once. */
        std::string lock_screen_shown;
        Kind kind = Kind::Pagination;
        Target target = Target::Start;
        Anchor anchor;
        std::string fragment, xpointer;
        int dom_version = 0;
        std::optional<std::size_t> known_start;
        /** History hints are accepted only for the exact loaded part length. */
        std::optional<std::size_t> known_end;
        /** Resource copied from the current verified spine; permits cache lookup before reopening EPUB. */
        std::string chapter_resource;
        std::string root, image_resource, cache_name, relative_path, content_revision;
        DocumentMetadata identity;
        graphics::LayoutSpec layout;
        graphics::Page page;
        std::vector<CoverSource> covers;
        std::string cache_header;
        std::optional<PageCache> page_cache;
        std::shared_ptr<const ContentCacheEntry> content_cache;
        bool content_cache_write_needed = false;
        std::vector<SpineItem> spine;
        std::optional<ReadingRecord> record;
        std::shared_ptr<FileDigest> font_scan;
        /** SaveBookmarks: the list as loaded, the replacement and its next sequence. */
        std::optional<BookmarkList> bookmark_base;
        std::vector<Bookmark> bookmarks;
        std::uint64_t bookmark_next = 1;
        /** Pagination only. Without a session the job prepares and discards its own. */
        std::shared_ptr<PaginationSession> session;
        std::shared_ptr<ChapterSession> chapter_session;
        std::shared_ptr<DictionarySession> dictionary_session;
        /** relative_path names one dictionary below Dictionaries. A lookup may instead search
         * dictionary_sources in order, opening each at most once. Candidates are ordered best
         * first: exact before inflected forms, then longer selections. A later source replaces
         * a match only with a strictly better candidate unless dictionary_navigation is set.
         */
        std::vector<std::string> dictionary_sources;
        std::vector<dictionary::Candidate> dictionary_candidates;
        /** Lookup navigation: -1 or 1 stops at the first matching source in the supplied order. */
        int dictionary_navigation = 0;
        std::uint32_t dictionary_ordinal = 0;
        std::shared_ptr<const std::vector<dictionary::Field>> dictionary_definition;
        dictionary::TextCursor dictionary_cursor;
        std::uint32_t dictionary_seek_pages = 0;
        bool dictionary_replay = false;
        bool dictionary_reflow = false, dictionary_full_layout = false;
        // Split the page before this scalar anchor so both layouts resume at the same text.
        std::size_t dictionary_layout_anchor = 0;
        bool dictionary_entry_lookup = false;
        /** Re-prepare sources whose definitive failure this session remembers. */
        bool dictionary_retry = false;
        bool save_last_book = false, cover_crop = false, preview = false;
        /** LockScreen: prepare the disk cache without retaining a result canvas. */
        bool lock_screen_prepare = false;
#ifndef ESP_PLATFORM
        /** Host fault injection: reject lock-screen cache row buffers above this size; zero disables it. */
        std::size_t lock_cache_limit_for_test = 0;
        /** Host fault injection after validation, before cached rows are composed. */
        std::function<void()> lock_cache_validated_for_test;
        /** Host fault injection: reject new lock-screen canvases above this size; zero disables it. */
        std::size_t lock_canvas_limit_for_test = 0;
#endif
        /** LockScreen: an absolute deadline shared by all rotation candidates; zero uses the default budget. */
        std::chrono::steady_clock::time_point lock_screen_deadline{};
        bool progressive = false;
        /** Optional chapter preparation uses the worker's background admission budget. */
        bool chapter_prefetch = false;
        std::uint64_t generation = 0;
        PP_DIAGNOSTIC(std::uint64_t diagnostic_session = 0, diagnostic_layout = 0;)
        std::size_t chapter = 0, offset = 0;
        std::uint32_t part = 0;
    };
    struct Result {
        StorageUsage storage;
        StoreStatus clear_cache_status = StoreStatus::IoError;
        std::vector<std::uint32_t> starts;
        std::size_t offset = 0;
        bool part_done = false, last_part = false;
        std::string error, font_identity, xpointer;
        std::vector<std::string> lock_screens;
        std::vector<DictionaryItem> dictionaries;
        /** Lookup: definitive source-open failures, bounded by the requested sources. */
        std::vector<DictionaryItem> dictionary_failed_sources;
        dictionary::Result dictionary_status;
        dictionary::Info dictionary_info;
        std::optional<dictionary::Candidate> dictionary_match;
        std::vector<dictionary::Entry> dictionary_entries;
        std::vector<dictionary::Field> dictionary_fields;
        bool dictionary_complete = false, dictionary_more = false, dictionary_reused = false;
        /** Lookup: index of the matching source; partial when another source failed. */
        std::size_t dictionary_source = 0;
        bool dictionary_partial = false;
        bool dictionary_missing_glyphs = false;
        graphics::Page dictionary_page;
        dictionary::TextCursor dictionary_next;
        dictionary::TextCursor dictionary_current;
        std::uint32_t dictionary_page_number = 0;
        std::string lock_screen;
        Anchor mapped_anchor;
        sync::PartPosition mapped_place;  // Valid with mapped_anchor after a successful MapDownload.
        double progress_fraction = 0;
        /** SleepCover retains completed pixels independently of the optional cache commit result. */
        std::unique_ptr<graphics::Canvas> image;
        std::vector<std::size_t> failed_images;
        PP_DIAGNOSTIC(std::uint32_t decoded_images = 0;)
        /** Missing value: retryable failure. Empty pixels: confirmed absence. Nonempty pixels: decoded cover. */
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
        /** SaveBookmarks: the commit status. */
        StoreStatus bookmark_status = StoreStatus::IoError;
        /** Open: loaded list. SaveBookmarks: committed list, or reloaded list after ExternalChange. */
        BookmarkList bookmarks;
        bool last_book_saved = false;
        /** SleepCover: a valid pixel or negative cache entry was read or successfully committed. */
        bool cache_finished = false;
        /** Cover/image preparation outcome; a usable image may coexist with an optional cache I/O failure.
         * LibraryCovers reports the last failed item; covers retains each item's availability.
         */
        ContentFailureKind failure_kind = ContentFailureKind::None;
        bool retryable_failure() const { return content_failure_is_retryable(failure_kind); }
        /** LockScreen: Request::lock_screen_shown was committed as the rotation cursor. */
        bool cursor_saved = false;
        std::optional<PageCache> page_cache;
        off_t file_size = 0;
        time_t file_modified = 0;
        ino_t file_inode = 0;
    };
    explicit ContentJob(Request request);
    /** Requests cancellation and waits for completion; destruction can block on in-flight I/O. */
    ~ContentJob();
    ContentJob(const ContentJob&) = delete;
    ContentJob& operator=(const ContentJob&) = delete;
#ifdef ESP_PLATFORM
    /**
     * Reserve one firmware-lifetime internal stack before networking starts. Call only on the
     * application owner task, as for start(). The worker sleeps when idle and borrows at most
     * one running job and one queued job; each owner must retain its job until done().
     * Failure leaves no worker resources allocated; start() can retry initialization.
     */
    static StartFailure prepare_worker();
#endif
    /**
     * Starts or enqueues this job from its owner task. True means accepted, not completed.
     * Retain the job and its borrowed resources until done(). False means already started
     * or a resource/start failure; start_failure() describes failures of a new start attempt.
     */
    bool start();
    StartFailure start_failure() const { return start_failure_; }
#ifndef ESP_PLATFORM
    /** Fail the next count starts of kind on the calling host thread; count=0 clears injection. */
    static void fail_starts_for_test(Kind kind, unsigned count, StartFailure failure);
    /** Delay jobs of kind started on the calling host thread; cancellation interrupts the delay. */
    static void delay_starts_for_test(Kind kind, std::chrono::milliseconds delay);
#endif
    bool started() const { return started_; }
    /** Requests cooperative cancellation; does not confirm completion or release borrowed resources. */
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    /** Requests a cooperative parsing/lookup pause; true acknowledges a checkpoint. Cancellation releases it. */
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
    /**
     * Waits for a started job to finish, without a timeout; returns immediately if never started.
     * In-flight I/O may delay completion even after cancel(). Keep this wait out of input and
     * drawing callbacks; poll done() before consuming result or releasing borrowed resources.
     */
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
    // Only the owner changes requests; each odd generation requires a fresh worker acknowledgement.
    std::atomic<std::uint32_t> pause_generation_{0}, paused_generation_{0};
    bool started_ = false;
    StartFailure start_failure_ = StartFailure::None;
#ifndef ESP_PLATFORM
    std::thread thread_;
#endif
};
} // namespace pico_page
