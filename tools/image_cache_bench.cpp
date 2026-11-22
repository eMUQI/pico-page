#include "../firmware/components/reader_app/content_job.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <unistd.h>

namespace {
constexpr std::size_t canvas_bytes = 342U * 1216;
std::atomic<bool> measuring{false};
std::atomic<std::size_t> large_allocations{0}, large_bytes{0};
void check(bool okay, const char* message) {
    if (!okay) throw std::runtime_error(message);
}
struct TemporaryCard {
    std::filesystem::path root;
    TemporaryCard() {
        auto pattern = (std::filesystem::temp_directory_path() / "pico-image-bench-XXXXXX").string();
        const auto directory = mkdtemp(pattern.data());
        check(directory != nullptr, "Cannot create temporary card");
        root = directory;
    }
    ~TemporaryCard() { std::error_code error; std::filesystem::remove_all(root, error); }
};
}

void* operator new(std::size_t size) {
    void* value = std::malloc(size ? size : 1);
    if (!value) throw std::bad_alloc();
    if (measuring.load(std::memory_order_relaxed) && size >= canvas_bytes) {
        large_allocations.fetch_add(1, std::memory_order_relaxed);
        large_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    return value;
}
void operator delete(void* value) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
#endif

int main(int argc, char** argv) try {
    using namespace pico_page;
    check(argc == 2 || argc == 3, "Usage: image_cache_bench <image-cover.epub> [samples=40]");
    const std::string count = argc == 3 ? argv[2] : "40";
    std::size_t used = 0;
    const auto samples = std::stoul(count, &used);
    check(used == count.size() && samples > 0 && samples <= 10000, "Invalid sample count");
    TemporaryCard card;
    ContentJob::Request request;
    request.kind = ContentJob::Kind::Image;
    request.root = card.root.string();
    request.path = (card.root / "book.epub").string();
    std::filesystem::copy_file(argv[1], request.path);
    EpubDocument document;
    std::string error;
    check(document.open(request.path, error), "Cannot open fixture");
    request.identity = document.metadata();
    request.layout.width = 684; request.layout.height = 1216;
    graphics::ImageBox box{};
    box.resource = "EPUB/cover.png"; box.width = 684; box.height = 1216;
    request.page.images.push_back(box);
    request.cache_name = image_cache_key(request.identity.book_id, box.resource, request.layout);
    std::string expected;
    std::cout << "scenario,sample,elapsed_us,canvas_sized_allocations,canvas_sized_requested_bytes,pixel_md5\n";
    for (unsigned long sample = 0; sample < samples + 2; ++sample) {
        for (const bool cold : {true, false}) {
            if (cold) std::filesystem::remove(card.root / "PicoPage/cache" / request.cache_name);
            ContentJob job(request);
            const auto decodes = graphics::image_decode_count();
            large_allocations = 0; large_bytes = 0;
            const auto start = std::chrono::steady_clock::now();
            measuring = true;
            const bool started = job.start();
            job.wait();
            measuring = false;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (!job.result.error.empty()) std::cerr << job.result.error << '\n';
            check(started && job.result.error.empty() && job.result.image && job.result.failed_images.empty(),
                  "Image job failed");
            check(graphics::image_decode_count() == decodes + (cold ? 1 : 0), "Unexpected decode count");
            const auto& pixels = job.result.image->pixels();
            const auto digest = md5_hex(std::string_view(reinterpret_cast<const char*>(pixels.data()), pixels.size()));
            if (expected.empty()) expected = digest;
            check(digest == expected, "Cold/hot pixels differ");
            if (sample >= 2) {
                std::cout << (cold ? "cold" : "hot") << ',' << sample - 1 << ',' << elapsed << ','
                          << large_allocations << ',' << large_bytes << ',' << digest << '\n';
                std::cerr << "reader_perf: stage=host_image_" << (cold ? "cold" : "hot")
                          << " us=" << elapsed << " min_us=0\n";
            }
        }
    }
    return 0;
} catch (const std::exception& error) {
    measuring = false;
    std::cerr << error.what() << '\n';
    return 1;
}
