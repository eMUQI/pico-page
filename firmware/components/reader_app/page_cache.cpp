#include "page_cache.h"
#include "pico_page/stage_timer.h"
#include <algorithm>
#include <cstring>
namespace pico_page {
namespace { constexpr std::uint32_t uncounted = 0xffffffffU; }
std::vector<bool> page_linearity(const std::vector<SpineItem>& spine) {
    std::vector<bool> linear;
    linear.reserve(spine.size());
    for (const auto& item : spine) linear.push_back(item.linear);
    return linear;
}
std::optional<PageCache> read_page_cache(const FileStore& store, const std::string& name, const std::string& header,
    const std::vector<bool>& linear, const std::function<bool()>& stop) {
    const StageTimer timer{"pages_cache_read"};
    if (stop && stop()) return std::nullopt;
    std::string cached;
    if (linear.size() > kPageIndexBytes / sizeof(PageIndices::value_type)) return std::nullopt;
    const auto index_bytes = linear.size() * sizeof(PageIndices::value_type);
    const auto admit = [&](std::size_t encoded) {
        constexpr auto part_header_expansion = sizeof(std::vector<std::uint32_t>) / sizeof(std::uint32_t);
        const auto payload_bytes = encoded > header.size() + 32 ? encoded - header.size() - 32 : 0;
        const auto expanded = std::min(kPageIndexBytes, index_bytes + payload_bytes * part_header_expansion);
        return chapter_memory_available(encoded + expanded);
    };
    if (store.load_cache(name, header.size() + kPageIndexBytes + 32, cached, admit) != StoreStatus::Ok ||
        cached.compare(0, header.size(), header) != 0)
        return std::nullopt;
    if (cached.size() < header.size() + 32) return std::nullopt;
    const auto checksum = cached.substr(cached.size() - 32);
    cached.resize(cached.size() - 32);
    if (md5_hex(cached) != checksum) return std::nullopt;
    PageIndices loaded(linear.size());
    std::vector<bool> finished;
    for (const auto item : linear) finished.push_back(!item);
    std::size_t at = header.size();
    std::size_t retained = loaded.capacity() * sizeof(decltype(loaded)::value_type);
    if (retained > kPageIndexBytes) return std::nullopt;
    const auto word = [&](std::uint32_t &value) {
        if (cached.size() - at < sizeof(value)) return false;
        std::memcpy(&value, cached.data() + at, sizeof(value));
        at += sizeof(value);
        return true;
    };
    for (std::size_t i = 0; i < linear.size(); ++i) {
        if (stop && stop()) return std::nullopt;
        std::uint32_t parts = 0;
        if (!word(parts)) return std::nullopt;
        if (parts == uncounted) continue;
        if (parts > (cached.size() - at) / sizeof(std::uint32_t) || (!linear[i] && parts)) return std::nullopt;
        if (parts > (kPageIndexBytes - retained) / sizeof(std::vector<std::uint32_t>)) return std::nullopt;
        retained += parts * sizeof(std::vector<std::uint32_t>);
        loaded[i].resize(parts);
        for (auto &starts : loaded[i]) {
            if (stop && stop()) return std::nullopt;
            std::uint32_t count = 0;
            if (!word(count) || count > (cached.size() - at) / sizeof(std::uint32_t)) return std::nullopt;
            if (count > (kPageIndexBytes - retained) / sizeof(std::uint32_t)) return std::nullopt;
            retained += count * sizeof(std::uint32_t);
            starts.resize(count);
            for (auto &start : starts)
                if (!word(start)) return std::nullopt;
            if (!std::is_sorted(starts.begin(), starts.end()) ||
                std::adjacent_find(starts.begin(), starts.end()) != starts.end()) return std::nullopt;
        }
        finished[i] = true;
    }
    if (at != cached.size()) return std::nullopt;

    if (stop && stop()) return std::nullopt;
    return PageCache{std::move(loaded), std::move(finished)};
}
std::optional<PageCache> read_page_cache(const FileStore& store, const std::string& name, const std::string& header,
    const std::vector<SpineItem>& spine, const std::function<bool()>& stop) {
    return read_page_cache(store, name, header, page_linearity(spine), stop);
}
bool write_page_cache(const FileStore& store, const std::string& name, const std::string& header,
    const PageIndices& pages, const std::vector<bool>& counted, const std::function<bool()>& stop) {
    const StageTimer timer{"pages_cache_commit"};
    if (pages.size() != counted.size() || (stop && stop())) return false;
    std::size_t payload = pages.size() * sizeof(std::uint32_t);
    if (payload > kPageIndexBytes) return false;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (!counted[i]) continue;
        for (const auto& starts : pages[i]) {
            const auto words = starts.size() + 1;
            if (words > (kPageIndexBytes - payload) / sizeof(std::uint32_t)) return false;
            payload += words * sizeof(std::uint32_t);
        }
    }
    std::string bytes;
    bytes.reserve(header.size() + payload + 32);
    bytes = header;
    const auto word = [&](std::uint32_t value) {
        bytes.append(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (stop && stop()) return false;
        word(counted[i] ? static_cast<std::uint32_t>(pages[i].size()) : uncounted);
        if (!counted[i]) continue;
        for (const auto &starts : pages[i]) {
            word(static_cast<std::uint32_t>(starts.size()));
            for (const auto start : starts) word(start);
        }
    }
    bytes += md5_hex(bytes);
    if (stop && stop()) return false;
    return store.save_cache(name, bytes) == StoreStatus::Ok;
}
}
