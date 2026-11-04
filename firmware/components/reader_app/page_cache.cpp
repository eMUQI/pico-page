#include "page_cache.h"
#include "pico_page/stage_timer.h"
#include <algorithm>
#include <cstring>
namespace pico_page {
namespace { constexpr std::uint32_t uncounted = 0xffffffffU; }
std::optional<PageCache> read_page_cache(const FileStore& store, const std::string& name, const std::string& header,
    const std::vector<SpineItem>& spine, const std::function<bool()>& stop) {
    const StageTimer timer{"pages_cache_read"};
    if (stop && stop()) return std::nullopt;
    std::string cached;
    if (store.load_cache(name, header.size() + kPageIndexBytes + 32, cached) != StoreStatus::Ok ||
        cached.compare(0, header.size(), header) != 0)
        return std::nullopt;
    if (cached.size() < header.size() + 32) return std::nullopt;
    const auto checksum = cached.substr(cached.size() - 32);
    cached.resize(cached.size() - 32);
    if (md5_hex(cached) != checksum) return std::nullopt;
    PageIndices loaded(spine.size());
    std::vector<bool> finished;
    for (const auto& item : spine) finished.push_back(!item.linear);
    std::size_t at = header.size();
    std::size_t retained = loaded.capacity() * sizeof(decltype(loaded)::value_type);
    if (retained > kPageIndexBytes) return std::nullopt;
    const auto word = [&](std::uint32_t &value) {
        if (cached.size() - at < sizeof(value)) return false;
        std::memcpy(&value, cached.data() + at, sizeof(value));
        at += sizeof(value);
        return true;
    };
    for (std::size_t i = 0; i < spine.size(); ++i) {
        if (stop && stop()) return std::nullopt;
        std::uint32_t parts = 0;
        if (!word(parts)) return std::nullopt;
        if (parts == uncounted) continue;
        if (parts > (cached.size() - at) / sizeof(std::uint32_t) || (!spine[i].linear && parts)) return std::nullopt;
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
