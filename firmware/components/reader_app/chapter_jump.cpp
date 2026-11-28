#include "chapter_jump.h"

#include <algorithm>
#include <limits>
#include <new>
#include <numeric>

namespace pico_page::chapter_jump {
namespace {

constexpr auto no_toc_entry = std::numeric_limits<std::uint16_t>::max();

std::uint64_t scale_percent(std::uint64_t total, std::uint64_t percent) {
    return total / 100 * percent + total % 100 * percent / 100;
}

std::optional<Position> counted_position(const std::vector<SpineItem>& spine, const PageIndices& pages,
                                         std::uint64_t percent) {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < spine.size(); ++i) {
        if (!spine[i].linear) continue;
        for (const auto& part : pages[i]) {
            if (part.size() > std::numeric_limits<std::uint64_t>::max() - total) return std::nullopt;
            total += part.size();
        }
    }
    if (!total) return std::nullopt;
    const auto target_page = total / 100 * percent + (total % 100 * percent + 99) / 100;
    auto remaining = target_page - 1;
    for (std::size_t i = 0; i < spine.size(); ++i) {
        if (!spine[i].linear) continue;
        for (std::uint32_t part = 0; part < pages[i].size() && part < kLastChapterPart; ++part) {
            const auto& starts = pages[i][part];
            if (remaining < starts.size())
                return Position{PositionKind::Start, i, part, starts[static_cast<std::size_t>(remaining)]};
            remaining -= starts.size();
        }
    }
    return std::nullopt;
}

}

std::optional<std::size_t> toc_index(std::size_t count, std::uint64_t ordinal) {
    if (!ordinal || ordinal > count) return std::nullopt;
    return static_cast<std::size_t>(ordinal - 1);
}

std::optional<Position> epub_position(const std::vector<SpineItem>& spine, const PageIndices* pages,
                                      std::uint64_t percent) {
    if (percent > 100) return std::nullopt;
    const auto linear = [](const SpineItem& item) { return item.linear; };
    if (percent == 100) {
        const auto last = std::find_if(spine.rbegin(), spine.rend(), linear);
        if (last == spine.rend()) return std::nullopt;
        return Position{PositionKind::End, static_cast<std::size_t>(last.base() - spine.begin() - 1),
                        kLastChapterPart, 0};
    }
    if (percent == 0) {
        const auto first = std::find_if(spine.begin(), spine.end(), linear);
        if (first == spine.end()) return std::nullopt;
        return Position{PositionKind::Start, static_cast<std::size_t>(first - spine.begin()), 0, 0};
    }
    if (pages && pages->size() == spine.size()) {
        if (const auto position = counted_position(spine, *pages, percent)) return position;
    }
    std::uint64_t total = 0;
    for (const auto& item : spine) {
        if (!item.linear) continue;
        const auto weight = std::max<std::uint32_t>(1, item.bytes);
        if (weight > std::numeric_limits<std::uint64_t>::max() - total) return std::nullopt;
        total += weight;
    }
    auto remaining = scale_percent(total, percent);
    for (std::size_t i = 0; i < spine.size(); ++i) {
        if (!spine[i].linear) continue;
        const auto weight = std::max<std::uint32_t>(1, spine[i].bytes);
        if (remaining < weight) return Position{remaining ? PositionKind::SourceByte : PositionKind::Start,
                                                i, 0, static_cast<std::uint32_t>(remaining)};
        remaining -= weight;
    }
    return std::nullopt;
}

std::optional<Position> txt_position(std::uint32_t bytes, const std::vector<std::uint32_t>& offsets,
                                     std::uint64_t percent) {
    if (!bytes || percent > 100) return std::nullopt;
    if (percent == 100) return Position{PositionKind::End, 0, kLastChapterPart, 0};
    const auto target = static_cast<std::uint32_t>(scale_percent(bytes, percent));
    if (!target) return Position{PositionKind::Start, 0, 0, 0};
    if (offsets.empty()) return Position{PositionKind::SourceByte, 0, target / kTxtPartBytes, target};
    if (offsets.front() != 0 || offsets.back() != bytes) return std::nullopt;
    const auto next = std::upper_bound(offsets.begin(), offsets.end(), target);
    return Position{PositionKind::SourceByte, 0, static_cast<std::uint32_t>(next - offsets.begin() - 1), target};
}

std::optional<std::size_t> containing_txt_heading(const std::vector<TxtHeading>& headings, std::uint32_t bytes,
                                                  const Position& position) {
    const auto byte = position.kind == PositionKind::End ? bytes :
        position.kind == PositionKind::SourceByte ? position.offset : 0U;
    const auto next = std::upper_bound(headings.begin(), headings.end(), byte,
        [](std::uint32_t value, const TxtHeading& heading) { return value < heading.byte_offset; });
    if (next == headings.begin()) return std::nullopt;
    return static_cast<std::size_t>(next - headings.begin() - 1);
}

EpubTocIndex::EpubTocIndex(const std::vector<TocEntry>& toc, const std::vector<SpineItem>& spine) {
    if (toc.empty() || toc.size() > no_toc_entry || spine.empty() || spine.size() > no_toc_entry) return;
    auto resource_order = std::unique_ptr<std::uint16_t[]>(new (std::nothrow) std::uint16_t[toc.size()]);
    if (!resource_order) return;
    entry_for_spine_.reset(new (std::nothrow) std::uint16_t[spine.size()]);
    if (!entry_for_spine_) return;
    spine_size_ = spine.size();
    auto* const begin = resource_order.get();
    auto* const end = begin + toc.size();
    std::iota(begin, end, std::uint16_t{0});
    std::sort(begin, end, [&](auto left, auto right) {
        const auto order = toc[left].resource.compare(toc[right].resource);
        return order ? order < 0 : left < right;
    });

    auto preceding_entry = no_toc_entry;
    for (std::size_t i = 0; i < spine_size_; ++i) {
        const auto& resource = spine[i].resource;
        const auto first = std::lower_bound(begin, end, resource,
            [&](auto entry, const auto& key) { return toc[entry].resource < key; });
        if (first == end || toc[*first].resource != resource) {
            entry_for_spine_[i] = preceding_entry;
            continue;
        }
        entry_for_spine_[i] = *first;
        const auto next = std::upper_bound(first, end, resource,
            [&](const auto& key, auto entry) { return key < toc[entry].resource; });
        preceding_entry = *(next - 1);
    }
}

std::optional<std::size_t> EpubTocIndex::containing_entry(std::size_t spine_index) const {
    if (!entry_for_spine_ || spine_index >= spine_size_) return std::nullopt;
    const auto entry = entry_for_spine_[spine_index];
    if (entry == no_toc_entry) return std::nullopt;
    return entry;
}

}
