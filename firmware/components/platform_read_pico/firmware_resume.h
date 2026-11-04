#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>

namespace pico_page::platform {

struct FirmwareRange {
    std::uint32_t first = 0, last = 0, size = 0;
    bool present = false, valid = false;

    void parse_content_range(std::string_view value) {
        if (present) { valid = false; return; }
        present = true;
        if (value.size() >= 64 || value.substr(0, 6) != "bytes ") return;
        value.remove_prefix(6);
        const auto number = [&](std::uint32_t& result) {
            result = 0;
            if (value.empty() || value.front() < '0' || value.front() > '9') return false;
            while (!value.empty() && value.front() >= '0' && value.front() <= '9') {
                const auto digit = static_cast<unsigned>(value.front() - '0');
                if (result > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) return false;
                result = result * 10 + digit;
                value.remove_prefix(1);
            }
            return true;
        };
        const auto separator = [&](char expected) {
            if (value.empty() || value.front() != expected) return false;
            value.remove_prefix(1);
            return true;
        };
        valid = number(first) && separator('-') && number(last) && separator('/') && number(size) &&
                value.empty() && first <= last && last < size;
    }

    bool accepts(int status, std::int64_t length, std::uint32_t offset, std::uint32_t expected_size) const {
        if (offset >= expected_size || length < 0 || (length > 0 && length != expected_size - offset)) return false;
        if (!offset) return status == 200 && !present;
        return status == 206 && present && valid && first == offset && last == expected_size - 1 && size == expected_size;
    }
};

class FirmwareRecovery {
public:
    static constexpr std::int64_t idle_us = 30'000'000;
    static constexpr std::int64_t total_us = 1'200'000'000;
    static constexpr unsigned max_reconnects = 3;

    explicit FirmwareRecovery(std::int64_t started_us) : deadline_us_(started_us + total_us) {}
    std::int64_t deadline_us() const { return deadline_us_; }
    std::int64_t read_deadline_us(std::int64_t now_us) const { return std::min(deadline_us_, now_us + idle_us); }
    unsigned reconnects() const { return reconnects_; }
    bool retry(std::int64_t now_us, bool cancelled) {
        if (cancelled || now_us >= deadline_us_ || reconnects_ == max_reconnects) return false;
        ++reconnects_;
        return true;
    }

private:
    std::int64_t deadline_us_;
    unsigned reconnects_ = 0;
};

}
