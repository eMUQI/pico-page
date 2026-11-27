#include "pico_page/store.h"
#include "store_internal.h"

#include <charconv>
#include <cstdio>
#include <sys/stat.h>

namespace pico_page {
namespace {

std::string source_header(const std::string& root, const std::string& path) {
    if (!store_internal::relative_path(path)) return {};
    struct stat info{};
    if (stat((root + "/" + path).c_str(), &info) || !S_ISREG(info.st_mode)) return {};
    return path + "\n" + std::to_string(info.st_size) + " " +
           std::to_string(static_cast<long long>(info.st_mtime)) + "\n";
}

std::uint64_t signature(const std::string& header, const ReadingRecord& record) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto byte = [&](unsigned char value) { hash = (hash ^ value) * 1099511628211ULL; };
    const auto number = [&](std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) { byte(static_cast<unsigned char>(value)); value >>= 8; }
    };
    const auto field = [&](const std::string& value) {
        number(value.size());
        for (auto character : value) byte(static_cast<unsigned char>(character));
    };
    field(header); field(record.book_id); field(record.content_revision); field(record.writer_id);
    number(record.local_revision); field(record.anchor.resource); field(record.anchor.node);
    number(record.anchor.offset);
    return hash;
}

}

std::string FileStore::position_cache_name(const std::string& path) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : path) hash = (hash ^ c) * 1099511628211ULL;
    char name[40];
    std::snprintf(name, sizeof(name), "positions/%016llx", static_cast<unsigned long long>(hash));
    return name;
}

std::uint64_t FileStore::position_signature(const std::string& path, const ReadingRecord& record) const {
    const auto header = source_header(root_, path);
    return header.empty() ? 0 : signature(header, record);
}

std::string FileStore::encode_cached_position(const std::string& path, const ReadingRecord& visible,
                                             const std::string& chapter, const std::string& percent,
                                             i18n::Language language) const {
    const auto header = source_header(root_, path);
    if (header.empty()) return {};
    return "PPPOS3\n" + std::string(i18n::language_code(language)) + "\n" + header + std::to_string(signature(header, visible)) + "\n" + chapter + "\n" + percent;
}

std::pair<std::string, std::string> FileStore::recalled_position(const std::string& path,
                                                                i18n::Language language) const {
    return read_cached_position(path, language, nullptr);
}

std::pair<std::string, std::string> FileStore::read_cached_position(const std::string& path,
                                                                   std::optional<i18n::Language> language,
                                                                   const ReadingRecord* required) const {
    const auto header = source_header(root_, path);
    if (header.empty()) return {};
    std::string bytes;
    if (load_cache(position_cache_name(path), 7 + 8 + header.size() + 21 + 512, bytes) != StoreStatus::Ok) return {};
    const bool signed_position = bytes.compare(0, 7, "PPPOS3\n") == 0;
    if (!signed_position && (required || bytes.compare(0, 7, "PPPOS2\n") != 0)) return {};
    const auto language_end = bytes.find('\n', 7);
    if (language_end == std::string::npos) return {};
    i18n::Language cached_language;
    if (!i18n::parse_language(std::string_view(bytes).substr(7, language_end - 7), cached_language) ||
        (language && *language != cached_language) || bytes.compare(language_end + 1, header.size(), header) != 0)
        return {};
    auto start = language_end + 1 + header.size();
    if (signed_position) {
        const auto end = bytes.find('\n', start);
        if (end == std::string::npos) return {};
        std::uint64_t stored = 0;
        const auto parsed = std::from_chars(bytes.data() + start, bytes.data() + end, stored);
        if (parsed.ec != std::errc() || parsed.ptr != bytes.data() + end ||
            (required && signature(header, *required) != stored)) return {};
        start = end + 1;
    }
    const auto split = bytes.find('\n', start);
    if (split == std::string::npos || bytes.size() - start > 512) return {};
    return {bytes.substr(start, split - start), bytes.substr(split + 1)};
}

int FileStore::cached_progress_percent(const std::string& book_path, const ReadingRecord& required) const {
    const auto percent = read_cached_position(book_path, std::nullopt, &required).second;
    if (percent.size() < 2 || percent.back() != '%') return -1;
    int value;
    const auto end = percent.data() + percent.size() - 1;
    const auto parsed = std::from_chars(percent.data(), end, value);
    return parsed.ec == std::errc() && parsed.ptr == end && value >= 0 && value <= 100 ? value : -1;
}

}
