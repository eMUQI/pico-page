#pragma once
#include "pico_page/document.h"
#include "pico_page/graphics.h"
#include <atomic>
#include <memory>
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
    enum class Kind { Pagination, FontIdentity, Image };
    struct Request {
        std::string path, font;
        Kind kind = Kind::Pagination;
        std::string root, image_resource, cache_name;
        DocumentMetadata identity;
        graphics::LayoutSpec layout;
        std::uint64_t generation = 0;
        std::size_t chapter = 0, offset = 0;
        std::uint32_t part = 0;
    };
    struct Result {
        std::vector<std::uint32_t> starts;
        std::size_t offset = 0;
        bool part_done = false, last_part = false;
        std::string error, font_identity;
        std::unique_ptr<graphics::Canvas> image;
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
    void render_image();
    std::atomic<bool> cancelled_{false}, done_{false};
    bool started_ = false;
#ifndef ESP_PLATFORM
    std::thread thread_;
#endif
};
} // namespace pico_page
