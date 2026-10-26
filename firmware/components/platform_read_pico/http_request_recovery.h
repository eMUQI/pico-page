#pragma once

#include <cstdint>
#include <string_view>

namespace pico_page::platform {

/** One transport retry for a read-only request, within its original monotonic deadline. */
class HttpRequestRecovery {
public:
    static constexpr std::int64_t total_us = 25'000'000;
    static constexpr std::int64_t backoff_us = 250'000;

    HttpRequestRecovery(std::string_view method, std::int64_t started_us)
        : deadline_us_(started_us + total_us), read_only_(method == "GET") {}

    std::int64_t deadline_us() const { return deadline_us_; }

    bool retry(bool transport_failed, std::int64_t now_us, bool cancelled) {
        if (!read_only_ || retried_ || !transport_failed || cancelled ||
            now_us >= deadline_us_ - backoff_us) return false;
        retried_ = true;
        return true;
    }

private:
    std::int64_t deadline_us_;
    bool read_only_, retried_ = false;
};

} // namespace pico_page::platform
