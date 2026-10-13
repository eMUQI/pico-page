#pragma once
#include "pico_page/document.h"
#include "pico_page/diagnostics.h"
#include "page_cache.h"
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
inline constexpr char kTxtGb18030Revision[] = "txt-gb18030-v1";
// Library card thumbnails are cropped to fill this frame; the size is part of the cache entry name.
inline constexpr int kLibraryCoverWidth = 184, kLibraryCoverHeight = 250;
inline constexpr auto kLockScreenPrepareBudget = std::chrono::seconds(20);
std::size_t readable_offset(const Chapter& source, std::size_t start);
const DocumentImage* image_at(const Chapter& source, std::size_t start);
graphics::Page plan_content(graphics::Font& font, const Chapter& source, std::size_t start,
                            const graphics::LayoutSpec& spec, bool collect,
                            const std::function<bool()>& stop = {});

std::string image_cache_key(const std::string& book_id, const std::string& resource, const graphics::LayoutSpec& spec);

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

/** A single job owns its parser and font, or borrows them from a PaginationSession. Only done() permits access to its result. */
class ContentJob {
public:
    enum class Kind { Pagination, FontIdentity, Image, ImageViewer, Open, Chapter, SaveProgress, LibraryCovers, PageCacheRead, PageCacheWrite, Inspect, SleepCover, MapUpload, MapDownload, SaveBookmarks, LockScreenList, LockScreen, SaveLockScreen };
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
        Kind kind = Kind::Pagination;
        Target target = Target::Start;
        Anchor anchor;
        std::string fragment, xpointer;
        int dom_version = 0;
        std::optional<std::size_t> known_start;
        std::string root, image_resource, cache_name, relative_path, content_revision;
        DocumentMetadata identity;
        graphics::LayoutSpec layout;
        graphics::Page page;
        std::vector<CoverSource> covers;
        std::string cache_header;
        std::optional<PageCache> page_cache;
        std::vector<SpineItem> spine;
        std::optional<ReadingRecord> record;
        std::shared_ptr<FileDigest> font_scan;
        /** SaveBookmarks: the list as loaded, the replacement and its next sequence. */
        std::optional<BookmarkList> bookmark_base;
        std::vector<Bookmark> bookmarks;
        std::uint64_t bookmark_next = 1;
        /** Pagination only. Without a session the job prepares and discards its own. */
        std::shared_ptr<PaginationSession> session;
        bool save_last_book = false, cover_crop = false, preview = false;
        /** LockScreen: prepare the disk cache without retaining a result canvas. */
        bool lock_screen_prepare = false;
        /** LockScreen: an absolute deadline shared by all rotation candidates; zero uses the default budget. */
        std::chrono::steady_clock::time_point lock_screen_deadline{};
        bool progressive = false;
        std::uint64_t generation = 0;
        PP_DIAGNOSTIC(std::uint64_t diagnostic_session = 0, diagnostic_layout = 0;)
        std::size_t chapter = 0, offset = 0;
        std::uint32_t part = 0;
    };
    struct Result {
        std::vector<std::uint32_t> starts;
        std::size_t offset = 0;
        bool part_done = false, last_part = false;
        std::string error, font_identity, xpointer;
        std::vector<std::string> lock_screens;
        std::string lock_screen;
        Anchor mapped_anchor;
        sync::PartPosition mapped_place;  // Valid with mapped_anchor after a successful MapDownload.
        double percentage = 0;
        std::unique_ptr<graphics::Canvas> image;
        std::vector<std::size_t> failed_images;
        PP_DIAGNOSTIC(std::uint32_t decoded_images = 0;)
        std::vector<std::vector<std::uint8_t>> covers;
        std::unique_ptr<EpubDocument> document;
        Chapter chapter;
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
        /** LockScreen: cancellation, timeout or resource failure permits an idle retry. */
        bool retryable_failure = false;
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
    /**
     * Reserve one firmware-lifetime internal stack before networking starts. Call only on the
     * application owner task, as for start(). The worker sleeps when idle and borrows at most
     * one running job and one queued job; each owner must retain its job until done().
     * Failure leaves no worker resources allocated; start() can retry initialization.
     */
    static StartFailure prepare_worker();
#endif
    bool start();
    StartFailure start_failure() const { return start_failure_; }
#ifndef ESP_PLATFORM
    /** Fail the next count starts of kind on the calling host thread; count=0 clears injection. */
    static void fail_starts_for_test(Kind kind, unsigned count, StartFailure failure);
    /** Delay jobs of kind started on the calling host thread; cancellation interrupts the delay. */
    static void delay_starts_for_test(Kind kind, std::chrono::milliseconds delay);
#endif
    bool started() const { return started_; }
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    bool done() const { return done_.load(std::memory_order_acquire); }
    void wait();
    const Request request;
    Result result;
private:
    void run();
#ifdef CONFIG_PICO_PAGE_PERF_METRICS
    void execute();
    diagnostics::Token measurement_{};
#endif
    void render_image();
    void prepare_lock_screen();
    void open_book();
    void prepare_cover();
    void map_position();
    void load_chapter();
    void library_covers();
    std::atomic<bool> cancelled_{false}, done_{false};
    bool started_ = false;
    StartFailure start_failure_ = StartFailure::None;
#ifndef ESP_PLATFORM
    std::thread thread_;
#endif
};
} // namespace pico_page
