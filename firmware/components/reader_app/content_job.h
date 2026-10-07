#pragma once
#include "pico_page/document.h"
#include "page_cache.h"
#include "pico_page/graphics.h"
#include "pico_page/store.h"
#include <sys/types.h>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>
#ifndef ESP_PLATFORM
#include <thread>
#endif

namespace pico_page {
std::size_t readable_offset(const Chapter& source, std::size_t start);
const DocumentImage* image_at(const Chapter& source, std::size_t start);
graphics::Page plan_content(graphics::Font& font, const Chapter& source, std::size_t start,
                            const graphics::LayoutSpec& spec, bool collect,
                            const std::function<bool()>& stop = {});

std::string image_cache_key(const std::string& book_id, const std::string& resource, const graphics::LayoutSpec& spec);

/** A single job owns its parser and font. Only done() permits access to its result. */
class ContentJob {
public:
    enum class Kind { Pagination, FontIdentity, Image, Open, Chapter, RasterPage, SaveProgress, LibraryCovers, PageCacheRead, PageCacheWrite, Inspect, SleepCover, MapUpload, MapDownload };
    enum class Target { Start, End, Before, Anchor, Fragment };
    struct CoverSource {
        std::string path, cache_name, cache_header;
        off_t size = 0;
        time_t modified = 0;
    };
    struct Request {
        std::string path, font;
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
        bool save_last_book = false, prefetch = false, cover_crop = false;
        std::uint64_t generation = 0;
        std::size_t chapter = 0, offset = 0;
        std::uint32_t part = 0;
    };
    struct Result {
        std::vector<std::uint32_t> starts;
        std::size_t offset = 0;
        bool part_done = false, last_part = false;
        std::string error, font_identity, xpointer;
        Anchor mapped_anchor;
        double percentage = 0;
        std::unique_ptr<graphics::Canvas> image;
        std::vector<std::vector<std::uint8_t>> covers;
        std::unique_ptr<EpubDocument> document;
        Chapter chapter;
        graphics::Page page;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> history;
        std::size_t chapter_index = 0;
        ConfigResult configuration;
        ProgressResult progress;
        bool last_book_saved = false, cache_finished = false;
        std::optional<PageCache> page_cache;
        off_t file_size = 0;
        time_t file_modified = 0;
        ino_t file_inode = 0;
    };
    explicit ContentJob(Request request);
    ~ContentJob();
    ContentJob(const ContentJob&) = delete;
    ContentJob& operator=(const ContentJob&) = delete;
    bool start();
    bool started() const { return started_; }
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    bool done() const { return done_.load(std::memory_order_acquire); }
    void wait();
    const Request request;
    Result result;
private:
    void run();
    void render_image(const std::string& resource, const std::string& cache_name);
    void prepare_chapter_page();
    void open_book();
    void prepare_cover();
    void map_position();
    void load_chapter();
    void raster_page();
    void library_covers();
    std::atomic<bool> cancelled_{false}, done_{false};
    bool started_ = false;
#ifndef ESP_PLATFORM
    std::thread thread_;
#endif
};
} // namespace pico_page
