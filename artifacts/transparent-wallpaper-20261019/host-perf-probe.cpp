#include "content_job.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <vector>
using namespace pico_page;
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string root = argv[1];
    std::vector<long long> times[3];
    for (int round = 0; round < 5; ++round) {
        assert(FileStore(root).clear_cache() == StoreStatus::Ok);
        for (int mode = 0; mode < 3; ++mode) {
            ContentJob::Request request;
            request.kind = ContentJob::Kind::LockScreen; request.root = root;
            request.sleep_image = "01.png"; request.sleep_overlay = mode == 2;
            if (request.sleep_overlay) {
                request.lock_background = std::make_shared<graphics::Canvas>();
                request.lock_background->clear(7);
            }
            const auto before = graphics::image_decode_count();
            const auto start = std::chrono::steady_clock::now();
            ContentJob job(std::move(request)); assert(job.start()); job.wait();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
            assert(job.result.image && job.result.error.empty() && job.result.cache_finished);
            assert(graphics::image_decode_count() - before == (mode == 0 ? 1 : 0));
            assert(job.result.image->pixel(600, 800) == (mode == 2 ? 7 : 15));
            times[mode].push_back(us);
            std::cout << "round=" << round + 1 << " mode=" << mode << " us=" << us << " decodes=" << graphics::image_decode_count()-before << '\n';
        }
    }
    for (int mode = 0; mode < 3; ++mode) {
        std::sort(times[mode].begin(), times[mode].end());
        std::cout << "summary mode=" << mode << " n=5 p50_us=" << times[mode][2] << " p95_us=" << times[mode][4] << " max_us=" << times[mode][4] << '\n';
    }
    std::cout << "cache_bytes=" << std::filesystem::file_size(root + "/PicoPage/cache/lockscreens/current") << '\n';
}
