#pragma once
#include <cstdint>
#include <limits>

namespace pico_page {
constexpr bool usb_block_range(std::uint32_t sectors, std::uint32_t lba,
                               std::uint32_t offset, std::uint32_t size) {
    const auto begin = std::uint64_t(lba) * 512 + offset;
    const auto capacity = std::uint64_t(sectors) * 512;
    return size <= std::uint32_t(std::numeric_limits<std::int32_t>::max()) &&
           begin <= capacity && size <= capacity - begin;
}
}
