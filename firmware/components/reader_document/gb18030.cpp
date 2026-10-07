#include "gb18030.h"

#include <algorithm>
#include <cstddef>

namespace pico_page::gb18030 {
namespace {

struct Range { std::uint32_t first, last, scalar; };

// Mapping values are generated from Python's GB18030 codec by tools/generate_gb18030_table.py.
// The Python release, the reproducibility check and the license review are recorded in
// docs/dependency-review.md.
#include "gb18030_data.inc"

}  // namespace

Decoder::Result Decoder::feed(std::uint8_t byte, char32_t& scalar) {
    if (count_ == 0) {
        if (byte < 0x80) { scalar = byte; return Result::Complete; }
        if (byte < 0x81 || byte > 0xfe) return Result::Invalid;
        bytes_[count_++] = byte;
        return Result::Pending;
    }
    if (count_ == 1) {
        if (byte >= 0x30 && byte <= 0x39) {
            bytes_[count_++] = byte;
            return Result::Pending;
        }
        if (byte < 0x40 || byte == 0x7f || byte > 0xfe) return Result::Invalid;
        const auto trail = byte < 0x7f ? byte - 0x40 : byte - 0x41;
        scalar = kTwoByte[(bytes_[0] - 0x81) * 190 + trail];
        count_ = 0;
        return scalar ? Result::Complete : Result::Invalid;
    }
    if (count_ == 2) {
        if (byte < 0x81 || byte > 0xfe) return Result::Invalid;
        bytes_[count_++] = byte;
        return Result::Pending;
    }
    if (byte < 0x30 || byte > 0x39) return Result::Invalid;
    const auto pointer = (((bytes_[0] - 0x81) * 10U + bytes_[1] - 0x30) * 126U +
                          bytes_[2] - 0x81) * 10U + byte - 0x30;
    count_ = 0;
    const auto it = std::upper_bound(std::begin(kFourByteRanges), std::end(kFourByteRanges), pointer,
        [](std::uint32_t value, const Range& range) { return value < range.first; });
    if (it == std::begin(kFourByteRanges)) return Result::Invalid;
    const auto& range = *(it - 1);
    if (pointer > range.last) return Result::Invalid;
    scalar = range.scalar + pointer - range.first;
    return Result::Complete;
}

bool decode(std::string_view bytes, std::u32string& text) {
    Decoder decoder;
    for (const unsigned char byte : bytes) {
        char32_t scalar = 0;
        const auto result = decoder.feed(byte, scalar);
        if (result == Decoder::Result::Invalid || (result == Decoder::Result::Complete && scalar == 0))
            return false;
        if (result == Decoder::Result::Complete) text.push_back(scalar);
    }
    return !decoder.pending();
}

}  // namespace pico_page::gb18030
