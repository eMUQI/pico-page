#pragma once
#include <cstdint>
constexpr int ESP_OK = 0;
constexpr std::uint16_t PMU_CMD_TIME_GET = 0x0007, PMU_CMD_TIME_SYNC = 0x0005;
constexpr std::uint16_t PMU_STATUS_OK = 0;
struct pmu_snapshot_t {
    bool time_ok = false;
    std::uint8_t time_synced = 0;
    std::uint32_t unix_sec = 0;
    std::uint16_t last_op_status = 0;
};
int read_pico_pmu_cmd(std::uint16_t, const std::uint8_t*, std::uint8_t);
const pmu_snapshot_t* read_pico_pmu_get();
