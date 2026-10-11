#include "rtc_clock.h"
#include "read_pico_pmu.h"
#include <cassert>
#include <ctime>
#include <string>
#include <vector>

namespace rtc = pico_page::platform::rtc_clock;
namespace {
constexpr std::time_t epoch = 1791676800;
std::time_t system_time = 0;
pmu_snapshot_t snapshot;
int get_error = 0, sync_error = 0, set_error = 0, set_calls = 0;
std::uint16_t status = PMU_STATUS_OK;
std::uint32_t written = 0;
std::int64_t readback_delta = 0;
std::vector<std::uint16_t> commands;
std::string message;
void reset() {
    rtc::persist();
    system_time = 0;
    snapshot = {true, 1, static_cast<std::uint32_t>(epoch), PMU_STATUS_OK};
    get_error = sync_error = set_error = set_calls = 0;
    status = PMU_STATUS_OK;
    written = 0;
    readback_delta = 0;
    commands.clear();
    message.clear();
}
void synchronized(std::time_t seconds = epoch) {
    timeval value{seconds, 0};
    system_time = seconds;
    rtc::synchronized(&value);
}
}

extern "C" std::time_t time(std::time_t* output) noexcept(noexcept(::time(nullptr))) {
    if (output) *output = system_time;
    return system_time;
}
extern "C" int settimeofday(const timeval* value, const struct timezone*) noexcept(noexcept(::settimeofday(nullptr, nullptr))) {
    ++set_calls;
    if (set_error) return set_error;
    system_time = value->tv_sec;
    return 0;
}
void rtc_test_log(const char*, const char* value) { message = value; }
const pmu_snapshot_t* read_pico_pmu_get() { return &snapshot; }
int read_pico_pmu_cmd(std::uint16_t command, const std::uint8_t* payload, std::uint8_t size) {
    commands.push_back(command);
    snapshot.last_op_status = status;
    if (command == PMU_CMD_TIME_GET) {
        assert(!payload && size == 0);
        return get_error;
    }
    assert(command == PMU_CMD_TIME_SYNC && size == 4);
    written = static_cast<std::uint32_t>(payload[0]) | (static_cast<std::uint32_t>(payload[1]) << 8) |
              (static_cast<std::uint32_t>(payload[2]) << 16) | (static_cast<std::uint32_t>(payload[3]) << 24);
    snapshot.unix_sec = static_cast<std::uint32_t>(written + readback_delta);
    return sync_error;
}

int main() {
    reset(); rtc::restore();
    assert(system_time == epoch && set_calls == 1 && commands.size() == 1);
    reset(); system_time = epoch + 10; rtc::restore();
    assert(system_time == epoch + 10 && commands.empty());
    for (int scenario = 0; scenario < 7; ++scenario) {
        reset();
        if (scenario == 0) get_error = -1;
        if (scenario == 1) snapshot.time_ok = false;
        if (scenario == 2) snapshot.time_synced = 0;
        if (scenario == 3) snapshot.unix_sec = 1704067199;
        if (scenario == 4) snapshot.unix_sec = 4102444800U;
        if (scenario == 5) status = 1;
        if (scenario == 6) snapshot.time_synced = 2;
        rtc::restore(); assert(system_time == 0 && set_calls == 0);
    }
    reset(); set_error = -1; rtc::restore();
    assert(system_time == 0 && set_calls == 1);
    for (const auto boundary : {1704067200LL, 4102444799LL}) {
        reset(); snapshot.unix_sec = boundary; rtc::restore(); assert(system_time == boundary);
        reset(); synchronized(boundary); rtc::persist(); assert(written == boundary);
    }
    reset(); rtc::synchronized(nullptr); synchronized(0); rtc::persist(); assert(commands.empty());
    reset(); synchronized(4102444800LL); rtc::persist(); assert(commands.empty());
    reset(); synchronized(); synchronized();
    assert(commands.empty());
    system_time += 120; rtc::persist();
    assert(written == epoch + 120 && commands.size() == 2);
    assert(message == "Network time saved to RTC");
    rtc::persist(); assert(commands.size() == 2);
    reset(); synchronized(); system_time = 0; rtc::persist(); assert(commands.empty());
    for (int scenario = 0; scenario < 5; ++scenario) {
        reset(); synchronized();
        if (scenario == 0) sync_error = -1;
        if (scenario == 1) get_error = -1;
        if (scenario == 2) status = 1;
        if (scenario == 3) readback_delta = -1;
        if (scenario == 4) readback_delta = 5;
        rtc::persist();
        assert(system_time == epoch && message != "Network time saved to RTC");
        const auto count = commands.size(); rtc::persist(); assert(commands.size() == count);
    }
}
