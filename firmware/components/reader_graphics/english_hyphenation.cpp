#include "pico_page/english_hyphenation.h"

#include <algorithm>
#include <array>

namespace pico_page::graphics {
namespace {

#include "third_party/tex_hyphen/patterns.inc"

constexpr std::uint8_t kWordBoundary = 0;
constexpr std::size_t kMinimumPrefix = 2;
constexpr std::size_t kMinimumSuffix = 3;
constexpr std::size_t kMaximumWordScalars = 63;
constexpr std::uint32_t kEdgeLabelMask = 0x1f;
constexpr std::uint32_t kFinalEdgeFlag = 1U << 5;

std::uint32_t edge_for(std::uint16_t first_edge, std::uint8_t letter) {
    if (!first_edge) return 0;
    for (std::size_t i = first_edge;; ++i) {
        const auto edge = kEnglishPatternEdges[i];
        const auto label = edge & kEdgeLabelMask;
        if (label == letter) return edge;
        if (label > letter || (edge & kFinalEdgeFlag)) return 0;
    }
}

}

std::uint64_t english_hyphenation_breaks(std::u32string_view word, EnglishHyphenation language) {
    if (language == EnglishHyphenation::None || word.size() < kMinimumPrefix + kMinimumSuffix ||
        word.size() > kMaximumWordScalars) return 0;

    std::array<std::uint8_t, kMaximumWordScalars + 2> padded;
    padded.front() = kWordBoundary;
    padded[word.size() + 1] = kWordBoundary;
    for (std::size_t i = 0; i < word.size(); ++i) {
        auto scalar = word[i];
        if (scalar >= U'A' && scalar <= U'Z') scalar += U'a' - U'A';
        if (scalar < U'a' || scalar > U'z') return 0;
        padded[i + 1] = static_cast<std::uint8_t>(scalar - U'a' + 1);
    }

    const auto root = language == EnglishHyphenation::Gb ? kBritishEnglishRoot : kAmericanEnglishRoot;
    const auto padded_size = word.size() + 2;
    std::array<std::uint8_t, kMaximumWordScalars + 3> scores{};
    for (std::size_t start = 0; start < padded_size; ++start) {
        auto children = root;
        for (std::size_t end = start; children && end < padded_size; ++end) {
            const auto edge = edge_for(children, padded[end]);
            children = static_cast<std::uint16_t>(edge >> 6);
            auto score = kEnglishPatternScores[edge >> 22];
            auto offset = start + static_cast<std::size_t>(score & 0xf);
            for (score >>= 4; score; score >>= 4, ++offset)
                scores[offset] = std::max(scores[offset], static_cast<std::uint8_t>(score & 0xf));
        }
    }

    std::uint64_t breaks = 0;
    for (std::size_t i = kMinimumPrefix; i <= word.size() - kMinimumSuffix; ++i)
        breaks |= static_cast<std::uint64_t>(scores[i + 1] & 1U) << i;
    return breaks;
}

}
