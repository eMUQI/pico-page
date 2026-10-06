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

/** A single job owns its parser and font. Only done() permits access to its result. */
class PaginationJob {
public:
    struct Request {
        std::string path, font;
        bool identity_only = false;
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
    };
    explicit PaginationJob(Request request);
    ~PaginationJob();
    PaginationJob(const PaginationJob&) = delete;
    PaginationJob& operator=(const PaginationJob&) = delete;
    bool start();
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    bool done() const { return done_.load(std::memory_order_acquire); }
    void wait();
    const Request request;
    Result result;
private:
    void run();
    std::atomic<bool> cancelled_{false}, done_{false};
    bool started_ = false;
#ifndef ESP_PLATFORM
    std::thread thread_;
#endif
};
} // namespace pico_page
