#include "firmware_resume.h"
#include "http_request_recovery.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

using pico_page::platform::FirmwareRange;
using pico_page::platform::FirmwareRecovery;

int main() {
    using pico_page::platform::HttpRequestRecovery;
    constexpr std::int64_t request_started_us = 1'000'000;
    HttpRequestRecovery read("GET", request_started_us);
    assert(!read.retry(false, request_started_us + 100, false));
    assert(!read.retry(true, request_started_us + 100, true));
    assert(read.retry(true, request_started_us + 10'000'000, false));
    assert(!read.retry(true, request_started_us + 11'000'000, false));
    assert(read.deadline_us() == request_started_us + 25'000'000);
    for (const auto method : {"PUT", "POST"}) {
        HttpRequestRecovery write(method, request_started_us);
        assert(!write.retry(true, request_started_us + 1, false));
    }
    HttpRequestRecovery late("GET", request_started_us);
    assert(!late.retry(true, late.deadline_us() - HttpRequestRecovery::backoff_us, false));
    assert(!late.retry(true, late.deadline_us(), false));
    assert(!late.retry(true, late.deadline_us() + 1, false));

    FirmwareRange range;
    assert(range.accepts(200, 8192, 0, 8192));
    assert(range.accepts(200, 0, 0, 8192));
    assert(!range.accepts(200, 8191, 0, 8192));
    assert(!range.accepts(206, 8192, 0, 8192));
    assert(!range.accepts(200, 4096, 4096, 8192));
    assert(!range.accepts(206, 4096, 4096, 8192));

    range.parse_content_range("bytes 4096-8191/8192");
    assert(range.valid && range.accepts(206, 4096, 4096, 8192));
    assert(range.accepts(206, 0, 4096, 8192));
    assert(!range.accepts(200, 4096, 4096, 8192));
    assert(!range.accepts(206, -1, 4096, 8192));
    assert(!range.accepts(206, 8192, 4096, 8192));
    assert(!range.accepts(206, 4096, 4095, 8192));
    assert(!range.accepts(206, 4096, 4096, 8193));
    assert(!range.accepts(200, 8192, 0, 8192));
    assert(!range.accepts(416, 0, 8192, 8192));
    range.parse_content_range("bytes 4096-8191/8192");
    assert(!range.valid && !range.accepts(206, 4096, 4096, 8192));
    range.parse_content_range("bytes 4096-8191/8192");
    assert(!range.valid);

    for (const char* value : {"bytes 0-4095/8192", "bytes 4096-8190/8192", "bytes 4096-8191/8193"}) {
        FirmwareRange response;
        response.parse_content_range(value);
        assert(response.valid && !response.accepts(206, 4096, 4096, 8192));
    }
    for (const std::string value : {"", "bytes */8192", "bytes -1-8191/8192", "bytes 8192-8191/8192",
             "bytes 4096-8192/8192", "bytes 4096-8191/0", "bytes 4294967296-4294967296/4294967297",
             "bytes 4096-8191/42949672960", "bytes 4096-8191/8192 trailing", "bytes 4096-8191/8192\r\n",
             "bytes 4096-8191/*", "bytes 4096 -8191/8192", "bytes 4096-8191/8192,0-1/8192"}) {
        FirmwareRange response;
        response.parse_content_range(value);
        assert(response.present && !response.valid && !response.accepts(206, 4096, 4096, 8192));
    }
    FirmwareRange oversized;
    oversized.parse_content_range(std::string(64, '1'));
    assert(oversized.present && !oversized.valid);
    FirmwareRange boundary;
    boundary.parse_content_range("bytes 4294967293-4294967294/4294967295");
    assert(boundary.valid && boundary.accepts(206, 2, UINT32_MAX - 2, UINT32_MAX));

    constexpr std::int64_t started_us = 1'000'000;
    FirmwareRecovery recovery(started_us);
    assert(recovery.read_deadline_us(started_us) == started_us + 30'000'000);
    assert(recovery.read_deadline_us(started_us + 29'000'000) == started_us + 59'000'000);
    assert(recovery.read_deadline_us(recovery.deadline_us() - 1) == recovery.deadline_us());
    assert(!recovery.retry(started_us, true) && recovery.reconnects() == 0);
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        assert(recovery.retry(started_us + attempt, false));
        assert(recovery.reconnects() == attempt);
        assert(recovery.read_deadline_us(started_us + attempt) <= recovery.deadline_us());
    }
    assert(!recovery.retry(started_us + 4, false) && recovery.reconnects() == 3);
    FirmwareRecovery expired(started_us);
    assert(!expired.retry(expired.deadline_us(), false) && expired.reconnects() == 0);
    assert(!expired.retry(expired.deadline_us() + 1, false));
    assert(expired.deadline_us() == started_us + 1'200'000'000);
    std::cout << "Firmware resume response and recovery checks passed\n";
}
