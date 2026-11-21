#include "pico_page/store.h"
#include "pico_page/clock_time.h"
#include "store_internal.h"

#include "cJSON.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#else
#include <sys/statvfs.h>
#endif

namespace pico_page {
namespace {
constexpr std::size_t progress_limit = 16384;
constexpr std::uint64_t import_limit = 64 * 1024 * 1024;
constexpr std::uint64_t lock_screen_limit = 2 * 1024 * 1024;
constexpr char clock_sync_path[] = "PicoPage/data/last-clock-sync.txt";
constexpr std::size_t clock_sync_limit = 64;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

std::int64_t parse_clock_sync(std::string_view bytes) {
    if (!bytes.empty() && bytes.back() == '\n') bytes.remove_suffix(1);
    if (!bytes.empty() && bytes.back() == '\r') bytes.remove_suffix(1);
    std::int64_t seconds = 0;
    const auto result = std::from_chars(bytes.data(), bytes.data() + bytes.size(), seconds);
    return result.ec == std::errc{} && result.ptr == bytes.data() + bytes.size() && clock_time::valid(seconds)
        ? seconds : 0;
}

int inspect(const char* path, struct stat* info) {
#ifdef ESP_PLATFORM
    // The mounted FAT filesystem has no symbolic links.
    return stat(path, info);
#else
    return lstat(path, info);
#endif
}

int open_no_follow(const std::string& path, int flags) {
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    return open(path.c_str(), flags, 0600);
}

bool sync_directory(const std::string& directory) {
#ifdef ESP_PLATFORM
    // ESP-IDF FAT VFS does not expose directory descriptors for fsync.
    (void)directory;
    return true;
#else
    const int fd = open_no_follow(directory, O_RDONLY);
    if (fd < 0) return false;
    const bool synced = fsync(fd) == 0;
    return close(fd) == 0 && synced;
#endif
}

bool text_value(const std::string& value, std::size_t maximum = 2048) {
    if (value.empty() || value.size() > maximum) return false;
    for (std::size_t index = 0; index < value.size();) {
        const auto leading = static_cast<unsigned char>(value[index++]);
        std::uint32_t scalar = leading;
        unsigned continuation = 0;
        if (leading < 0x80) {
            if (leading < 0x20 || leading == 0x7f) return false;
        } else if (leading >= 0xc2 && leading <= 0xdf) {
            scalar &= 0x1f; continuation = 1;
        } else if (leading >= 0xe0 && leading <= 0xef) {
            scalar &= 0x0f; continuation = 2;
        } else if (leading >= 0xf0 && leading <= 0xf4) {
            scalar &= 0x07; continuation = 3;
        } else return false;
        if (index + continuation > value.size()) return false;
        for (unsigned count = 0; count < continuation; ++count) {
            const auto byte = static_cast<unsigned char>(value[index++]);
            if ((byte & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (byte & 0x3f);
        }
        if ((continuation == 2 && scalar < 0x800) || (continuation == 3 && scalar < 0x10000) ||
            scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
    }
    return true;
}

bool valid_record(const ReadingRecord& record) {
    return text_value(record.book_id, 256) && text_value(record.content_revision, 256) &&
           text_value(record.writer_id, 256) && record.local_revision > 0 &&
           text_value(record.anchor.resource) && text_value(record.anchor.node);
}

bool fields(const cJSON* object, std::initializer_list<const char*> expected) {
    if (!cJSON_IsObject(object)) return false;
    std::size_t count = 0;
    for (auto* child = object->child; child; child = child->next) {
        bool known = false;
        for (const char* key : expected) {
            if (child->string && std::strcmp(child->string, key) == 0) known = true;
        }
        if (!known) return false;
        for (auto* earlier = object->child; earlier != child; earlier = earlier->next) {
            if (std::strcmp(earlier->string, child->string) == 0) return false;
        }
        ++count;
    }
    return count == expected.size();
}

bool string_field(const cJSON* object, const char* key, std::string& value) {
    const auto* field = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(field) || !field->valuestring) return false;
    value = field->valuestring;
    return true;
}

ProgressResult decode(const std::string& bytes, const std::string& book_id,
                      const std::string& content_revision) {
    ProgressResult result{StoreStatus::Invalid, {}, false, "invalid progress record"};
    if (bytes.find('\0') != std::string::npos || bytes.find("\\u0000") != std::string::npos) return result;
    Json object(cJSON_ParseWithLengthOpts(bytes.c_str(), bytes.size() + 1, nullptr, true), cJSON_Delete);
    if (!object) return result;
    const auto* schema = cJSON_GetObjectItemCaseSensitive(object.get(), "schema_version");
    if (!cJSON_IsNumber(schema) || !std::isfinite(schema->valuedouble) ||
        std::floor(schema->valuedouble) != schema->valuedouble) return result;
    if (schema->valuedouble == 2 &&
        fields(object.get(), {"schema_version", "state", "book_id", "content_revision"})) {
        std::string state, identity, revision;
        if (string_field(object.get(), "state", state) && state == "cleared" &&
            string_field(object.get(), "book_id", identity) && text_value(identity, 256) &&
            string_field(object.get(), "content_revision", revision) && text_value(revision, 256)) {
            if (identity != book_id || revision != content_revision)
                return {StoreStatus::IdentityMismatch, {}, false, "cleared record identity differs"};
            return {StoreStatus::Cleared, {}, false, {}};
        }
    }
    if (schema->valuedouble > 1) {
        result.status = StoreStatus::UnsupportedVersion;
        result.detail = "unsupported progress schema_version";
        return result;
    }
    if (schema->valuedouble != 1 ||
        !fields(object.get(), {"schema_version", "book_id", "content_revision", "writer_id", "local_revision", "anchor"})) return result;
    ReadingRecord record;
    std::string revision;
    const auto* anchor = cJSON_GetObjectItemCaseSensitive(object.get(), "anchor");
    if (!fields(anchor, {"resource", "node", "offset"}) ||
        !string_field(object.get(), "book_id", record.book_id) ||
        !string_field(object.get(), "content_revision", record.content_revision) ||
        !string_field(object.get(), "writer_id", record.writer_id) ||
        !string_field(object.get(), "local_revision", revision) ||
        !string_field(anchor, "resource", record.anchor.resource) ||
        !string_field(anchor, "node", record.anchor.node)) return result;
    const auto parsed = std::from_chars(revision.data(), revision.data() + revision.size(), record.local_revision);
    if (parsed.ec != std::errc{} || parsed.ptr != revision.data() + revision.size()) return result;
    const auto* offset = cJSON_GetObjectItemCaseSensitive(anchor, "offset");
    if (!cJSON_IsNumber(offset) || !std::isfinite(offset->valuedouble) || offset->valuedouble < 0 ||
        offset->valuedouble > std::numeric_limits<std::uint32_t>::max() ||
        std::floor(offset->valuedouble) != offset->valuedouble) return result;
    record.anchor.offset = static_cast<std::uint32_t>(offset->valuedouble);
    if (!valid_record(record)) return result;
    if (record.book_id != book_id || record.content_revision != content_revision) {
        return {StoreStatus::IdentityMismatch, {}, false, "book_id or content_revision differs"};
    }
    return {StoreStatus::Ok, record, false, {}};
}

std::string encode(const ReadingRecord& record) {
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object) return {};
    cJSON* anchor = cJSON_AddObjectToObject(object.get(), "anchor");
    if (!anchor || !cJSON_AddNumberToObject(object.get(), "schema_version", 1) ||
        !cJSON_AddStringToObject(object.get(), "book_id", record.book_id.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "content_revision", record.content_revision.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "writer_id", record.writer_id.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "local_revision", std::to_string(record.local_revision).c_str()) ||
        !cJSON_AddStringToObject(anchor, "resource", record.anchor.resource.c_str()) ||
        !cJSON_AddStringToObject(anchor, "node", record.anchor.node.c_str()) ||
        !cJSON_AddNumberToObject(anchor, "offset", record.anchor.offset)) return {};
    char* printed = cJSON_PrintUnformatted(object.get());
    if (!printed) return {};
    std::string bytes(printed);
    cJSON_free(printed);
    return bytes;
}

bool display_text(const std::string& value, std::size_t maximum) {
    return value.empty() || text_value(value, maximum);
}

bool known_fields(const cJSON* object, std::initializer_list<const char*> required,
                  std::initializer_list<const char*> optional) {
    if (!cJSON_IsObject(object)) return false;
    std::size_t found = 0;
    for (auto* child = object->child; child; child = child->next) {
        if (!child->string) return false;
        bool is_required = false, is_optional = false;
        for (const char* key : required) is_required = is_required || std::strcmp(child->string, key) == 0;
        for (const char* key : optional) is_optional = is_optional || std::strcmp(child->string, key) == 0;
        if (!is_required && !is_optional) return false;
        for (auto* earlier = object->child; earlier != child; earlier = earlier->next) {
            if (std::strcmp(earlier->string, child->string) == 0) return false;
        }
        if (is_required) ++found;
    }
    return found == required.size();
}

/** Decimal string of a nonzero 64-bit sequence; strings avoid the double precision of cJSON numbers. */
bool sequence_field(const cJSON* object, const char* key, std::uint64_t& value) {
    std::string digits;
    if (!string_field(object, key, digits) || digits.empty() || digits.size() > 20 || digits.front() == '0') return false;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() && value > 0;
}

bool valid_bookmark(const Bookmark& bookmark) {
    return bookmark.sequence > 0 && text_value(bookmark.anchor.resource) && text_value(bookmark.anchor.node) &&
           display_text(bookmark.chapter, 512) && display_text(bookmark.excerpt, 512) &&
           bookmark.percent >= -1 && bookmark.percent <= 100 &&
           (!bookmark.created_at || text_value(*bookmark.created_at, 64)) &&
           (!bookmark.time_source || text_value(*bookmark.time_source, 64)) &&
           bookmark.created_at.has_value() == bookmark.time_source.has_value();
}

bool valid_bookmarks(const std::vector<Bookmark>& bookmarks, std::uint64_t next_sequence) {
    if (bookmarks.size() > kMaxBookmarks || next_sequence == 0) return false;
    for (std::size_t i = 0; i < bookmarks.size(); ++i) {
        if (!valid_bookmark(bookmarks[i]) || bookmarks[i].sequence >= next_sequence ||
            (i && bookmarks[i - 1].sequence >= bookmarks[i].sequence)) return false;
    }
    return true;
}

bool json_nul_escape(const std::string& bytes) {
    for (auto at = bytes.find("u0000"); at != std::string::npos; at = bytes.find("u0000", at + 1)) {
        std::size_t backslashes = 0;
        while (backslashes < at && bytes[at - 1 - backslashes] == '\\') ++backslashes;
        if (backslashes % 2 == 1) return true;
    }
    return false;
}

BookmarkList decode_bookmarks(const std::string& bytes, const std::string& book_id,
                              const std::string& content_revision) {
    BookmarkList result;
    result.status = StoreStatus::Invalid;
    result.detail = "invalid bookmark file";
    if (bytes.find('\0') != std::string::npos || json_nul_escape(bytes)) return result;
    Json object(cJSON_ParseWithLengthOpts(bytes.c_str(), bytes.size() + 1, nullptr, true), cJSON_Delete);
    if (!object) return result;
    const auto* schema = cJSON_GetObjectItemCaseSensitive(object.get(), "schema_version");
    if (!cJSON_IsNumber(schema) || !std::isfinite(schema->valuedouble) ||
        std::floor(schema->valuedouble) != schema->valuedouble) return result;
    if (schema->valuedouble > 1) {
        result.status = StoreStatus::UnsupportedVersion;
        result.detail = "unsupported bookmark schema_version";
        return result;
    }
    std::string identity, revision;
    std::uint64_t next_sequence = 0;
    const auto* items = cJSON_GetObjectItemCaseSensitive(object.get(), "bookmarks");
    if (schema->valuedouble != 1 ||
        !fields(object.get(), {"schema_version", "book_id", "content_revision", "next_sequence", "bookmarks"}) ||
        !string_field(object.get(), "book_id", identity) || !text_value(identity, 256) ||
        !string_field(object.get(), "content_revision", revision) || !text_value(revision, 256) ||
        !sequence_field(object.get(), "next_sequence", next_sequence) || !cJSON_IsArray(items) ||
        static_cast<std::size_t>(cJSON_GetArraySize(items)) > kMaxBookmarks) return result;
    std::vector<Bookmark> bookmarks;
    for (const auto* item = items->child; item; item = item->next) {
        Bookmark bookmark;
        const auto* anchor = cJSON_GetObjectItemCaseSensitive(item, "anchor");
        if (!known_fields(item, {"sequence", "anchor", "chapter", "excerpt"}, {"percent", "created_at", "time_source"}) ||
            !sequence_field(item, "sequence", bookmark.sequence) || !fields(anchor, {"resource", "node", "offset"}) ||
            !string_field(anchor, "resource", bookmark.anchor.resource) ||
            !string_field(anchor, "node", bookmark.anchor.node) ||
            !string_field(item, "chapter", bookmark.chapter) || !string_field(item, "excerpt", bookmark.excerpt)) return result;
        const auto* offset = cJSON_GetObjectItemCaseSensitive(anchor, "offset");
        if (!cJSON_IsNumber(offset) || !std::isfinite(offset->valuedouble) || offset->valuedouble < 0 ||
            offset->valuedouble > std::numeric_limits<std::uint32_t>::max() ||
            std::floor(offset->valuedouble) != offset->valuedouble) return result;
        bookmark.anchor.offset = static_cast<std::uint32_t>(offset->valuedouble);
        if (const auto* percent = cJSON_GetObjectItemCaseSensitive(item, "percent")) {
            if (!cJSON_IsNumber(percent) || !std::isfinite(percent->valuedouble) || percent->valuedouble < 0 ||
                percent->valuedouble > 100 || std::floor(percent->valuedouble) != percent->valuedouble) return result;
            bookmark.percent = static_cast<int>(percent->valuedouble);
        }
        const std::pair<const char*, std::optional<std::string>*> optional_fields[] = {
            {"created_at", &bookmark.created_at}, {"time_source", &bookmark.time_source}};
        for (const auto& optional : optional_fields) {
            if (!cJSON_GetObjectItemCaseSensitive(item, optional.first)) continue;
            std::string value;
            if (!string_field(item, optional.first, value)) return result;
            *optional.second = std::move(value);
        }
        bookmarks.push_back(std::move(bookmark));
    }
    std::sort(bookmarks.begin(), bookmarks.end(),
              [](const Bookmark& a, const Bookmark& b) { return a.sequence < b.sequence; });
    if (!valid_bookmarks(bookmarks, next_sequence)) return result;
    if (identity != book_id || revision != content_revision) {
        result.status = StoreStatus::IdentityMismatch;
        result.foreign_book_id = identity;
        result.detail = "bookmark book_id or content_revision differs";
        return result;
    }
    result.status = StoreStatus::Ok;
    result.bookmarks = std::move(bookmarks);
    result.next_sequence = next_sequence;
    result.detail.clear();
    return result;
}

std::string encode_bookmarks(const std::string& book_id, const std::string& content_revision,
                             const std::vector<Bookmark>& bookmarks, std::uint64_t next_sequence) {
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object || !cJSON_AddNumberToObject(object.get(), "schema_version", 1) ||
        !cJSON_AddStringToObject(object.get(), "book_id", book_id.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "content_revision", content_revision.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "next_sequence", std::to_string(next_sequence).c_str())) return {};
    cJSON* items = cJSON_AddArrayToObject(object.get(), "bookmarks");
    if (!items) return {};
    for (const auto& bookmark : bookmarks) {
        cJSON* item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(items, item)) { cJSON_Delete(item); return {}; }
        cJSON* anchor = nullptr;
        if (!cJSON_AddStringToObject(item, "sequence", std::to_string(bookmark.sequence).c_str()) ||
            !(anchor = cJSON_AddObjectToObject(item, "anchor")) ||
            !cJSON_AddStringToObject(anchor, "resource", bookmark.anchor.resource.c_str()) ||
            !cJSON_AddStringToObject(anchor, "node", bookmark.anchor.node.c_str()) ||
            !cJSON_AddNumberToObject(anchor, "offset", bookmark.anchor.offset) ||
            !cJSON_AddStringToObject(item, "chapter", bookmark.chapter.c_str()) ||
            !cJSON_AddStringToObject(item, "excerpt", bookmark.excerpt.c_str()) ||
            (bookmark.percent >= 0 && !cJSON_AddNumberToObject(item, "percent", bookmark.percent)) ||
            (bookmark.created_at && !cJSON_AddStringToObject(item, "created_at", bookmark.created_at->c_str())) ||
            (bookmark.time_source && !cJSON_AddStringToObject(item, "time_source", bookmark.time_source->c_str())))
            return {};
    }
    char* printed = cJSON_Print(object.get());
    if (!printed) return {};
    std::string bytes(printed);
    cJSON_free(printed);
    return bytes;
}

std::string archive_prefix(const std::string& book_id) {
    std::string prefix;
    for (char ch : book_id) {
        if (prefix.size() == 8) break;
        const bool safe = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        prefix += safe ? ch : '_';
    }
    return prefix.empty() ? "unknown" : prefix;
}

bool rename_to_new(const std::string& root, const std::string& from, const std::string& to) {
    if (store_internal::safe_path(root, to) != StoreStatus::Missing) return false;
    const auto source = root + "/" + from, destination = root + "/" + to;
#ifdef ESP_PLATFORM
#if defined(CONFIG_FATFS_VFS_RENAME_REPLACES_DESTINATION)
    (void)source; (void)destination;
    return false;
#else
    if (rename(source.c_str(), destination.c_str()) != 0) return false;
#endif
#else
    // link() refuses an existing name atomically, unlike POSIX rename().
    if (link(source.c_str(), destination.c_str()) != 0 || unlink(source.c_str()) != 0) return false;
#endif
    return sync_directory(root + "/" + to.substr(0, to.rfind('/')));
}

StoreStatus read_slot(const std::string& root, const std::string& path, std::optional<std::string>& bytes) {
    std::string contents;
    const auto status = store_internal::read_file(root, path, kBookmarkFileLimit, contents);
    bytes.reset();
    if (status == StoreStatus::Ok) bytes = std::move(contents);
    return status == StoreStatus::Missing ? StoreStatus::Ok : status;
}

std::uint32_t resource_crc(std::uint32_t crc, const void* data, std::size_t size);

std::optional<SlotDigest> digest(const std::optional<std::string>& bytes) {
    if (!bytes) return std::nullopt;
    return SlotDigest{bytes->size(), resource_crc(0, bytes->data(), bytes->size())};
}

ProgressResult read_progress(const std::string& root, const std::string& path,
                             const std::string& book_id, const std::string& revision) {
    std::string bytes;
    const auto status = store_internal::read_file(root, path, progress_limit, bytes);
    if (status != StoreStatus::Ok) return {status, {}, false, path};
    auto result = decode(bytes, book_id, revision);
    if (result.status != StoreStatus::Ok) result.detail = path + ": " + result.detail;
    return result;
}

bool write_synced(const std::string& root, const std::string& path, const std::string& bytes,
                  std::string_view prefix = {}, std::string_view suffix = {}) {
    const auto safe = store_internal::safe_path(root, path);
    if (safe != StoreStatus::Ok && safe != StoreStatus::Missing) return false;
    const auto full = root + "/" + path;
    // Temporary files are replaced rather than truncated to avoid mutating hard links.
    if (safe == StoreStatus::Ok && unlink(full.c_str()) != 0) return false;
    const int fd = open_no_follow(full, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0) return false;
    for (const std::string_view part : {prefix, std::string_view(bytes), suffix}) {
        std::size_t written = 0;
        while (written < part.size()) {
            const auto count = write(fd, part.data() + written, part.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                close(fd);
                return false;
            }
            written += static_cast<std::size_t>(count);
        }
    }
    const bool synced = fsync(fd) == 0;
    return close(fd) == 0 && synced;
}

bool publish(const std::string& root, const std::string& temporary, const std::string& target) {
    const auto safe = store_internal::safe_path(root, target);
    if (safe != StoreStatus::Ok && safe != StoreStatus::Missing) return false;
    const auto full_target = root + "/" + target;
    // FatFs rename refuses an existing destination, so replacement requires a separate removal.
    if (safe == StoreStatus::Ok) {
        struct stat info{};
        if (inspect(full_target.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
            unlink(full_target.c_str()) != 0) return false;
    }
    return rename((root + "/" + temporary).c_str(), full_target.c_str()) == 0 &&
           sync_directory(root + "/" + target.substr(0, target.rfind('/')));
}

bool asset_path(const std::string& path) {
    if (path.size() > 512 || !store_internal::relative_path(path)) return false;
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    auto extension = path.substr(dot);
    for (auto& ch : extension) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (path.compare(0, 6, "Books/") == 0)
        return extension == ".epub" || extension == ".txt";
    if (path.compare(0, 6, "Fonts/") == 0)
        return extension == ".ttf" || extension == ".otf" || extension == ".txt";
    if (path.compare(0, 12, "LockScreens/") == 0) return valid_lock_screen_name(path.substr(12));
    if (path.compare(0, 13, "Dictionaries/") == 0)
        return extension == ".ifo" || extension == ".idx" || extension == ".dict" || extension == ".syn" ||
               extension == ".gz" || extension == ".dz";
    return false;
}

std::uint32_t resource_crc(std::uint32_t crc, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
#ifdef ESP_PLATFORM
    return esp_rom_crc32_le(crc, bytes, size);
#else
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
#endif
}

StoreStatus asset_digest(const std::string& root, const std::string& path,
                         std::uint64_t& size, std::uint32_t& crc) {
    size = 0; crc = 0;
    const auto safe = store_internal::safe_path(root, path);
    if (safe != StoreStatus::Ok) return safe;
    const int fd = open_no_follow(root + "/" + path, O_RDONLY);
    if (fd < 0) return StoreStatus::IoError;
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > import_limit) {
        close(fd); return StoreStatus::Invalid;
    }
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[4096]);
    if (!buffer) { close(fd); return StoreStatus::IoError; }
    while (true) {
        const auto count = read(fd, buffer.get(), 4096);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { close(fd); return StoreStatus::IoError; }
        if (!count) break;
        size += static_cast<std::uint64_t>(count);
        if (size > import_limit) { close(fd); return StoreStatus::Invalid; }
        crc = resource_crc(crc, buffer.get(), static_cast<std::size_t>(count));
    }
    const bool exact_size = size == static_cast<std::uint64_t>(info.st_size);
    return close(fd) == 0 && exact_size ? StoreStatus::Ok : StoreStatus::IoError;
}

StoreStatus remove_import_staging(const std::string& root, const std::string& path,
                                 std::string& error) {
    const auto temporary = path + ".import.tmp";
    const auto safe = store_internal::safe_path(root, temporary);
    if (safe == StoreStatus::Missing) return StoreStatus::Ok;
    struct stat info{};
    if (safe != StoreStatus::Ok || inspect((root + "/" + temporary).c_str(), &info) != 0 ||
        !S_ISREG(info.st_mode)) {
        error = "staging_unsafe"; return StoreStatus::UnsafePath;
    }
    if (unlink((root + "/" + temporary).c_str()) != 0 ||
        !sync_directory(root + "/" + path.substr(0, path.rfind('/')))) {
        error = "staging_cleanup_failed"; return StoreStatus::IoError;
    }
    return StoreStatus::Ok;
}

bool resource_parents(const std::string& root, const std::string& path) {
    std::size_t previous = std::string::npos;
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        const auto directory = path.substr(0, slash);
        const auto safe = store_internal::safe_path(root, directory);
        if (safe == StoreStatus::Missing) {
            if (mkdir((root + "/" + directory).c_str(), 0700) != 0) return false;
        } else if (safe != StoreStatus::Ok) return false;
        struct stat info{};
        if (inspect((root + "/" + directory).c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
        const auto parent = previous == std::string::npos ? root : root + "/" + path.substr(0, previous);
        if (!sync_directory(parent)) return false;
        previous = slash;
    }
    return true;
}

std::optional<std::uint64_t> tree_bytes(const std::string& root, const std::string& relative,
                                         bool books_only, const std::function<bool()>& stop) {
    unsigned remaining = 2000;
    std::uint64_t total = 0;
    std::vector<std::string> pending{relative};
    while (!pending.empty()) {
        if (stop && stop()) return std::nullopt;
        const auto directory = std::move(pending.back());
        pending.pop_back();
        DIR* entries = opendir((root + "/" + directory).c_str());
        if (!entries) return std::nullopt;
        bool complete = true;
        while (true) {
            if (stop && stop()) { complete = false; break; }
            errno = 0;
            const auto* item = readdir(entries);
            if (!item) { complete = errno == 0; break; }
            const std::string name = item->d_name;
            if (name == "." || name == "..") continue;
            const auto path = directory + "/" + name;
            struct stat info{};
            if (!remaining-- || path.size() > 1024 || inspect((root + "/" + path).c_str(), &info) != 0) {
                complete = false; break;
            }
            if (S_ISDIR(info.st_mode)) pending.push_back(path);
            else if (S_ISREG(info.st_mode) && (!books_only || asset_path("Books/" + name)))
                total += static_cast<std::uint64_t>(info.st_size);
        }
        if (closedir(entries) != 0) complete = false;
        if (!complete) return std::nullopt;
    }
    return total;
}

StoreStatus remove_children(const std::string& directory, unsigned depth, const std::function<bool()>& stop) {
    if (depth > 32) return StoreStatus::Invalid;
    DIR* entries = opendir(directory.c_str());
    if (!entries) return StoreStatus::IoError;
    StoreStatus status = StoreStatus::Ok;
    while (true) {
        if (stop && stop()) { status = StoreStatus::Cancelled; break; }
        errno = 0;
        auto* entry = readdir(entries);
        if (!entry) {
            if (errno) status = StoreStatus::IoError;
            break;
        }
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        const std::string path = directory + "/" + name;
        struct stat info{};
        if (inspect(path.c_str(), &info) != 0) { status = StoreStatus::IoError; break; }
        if (S_ISDIR(info.st_mode)) {
            status = remove_children(path, depth + 1, stop);
            if (status != StoreStatus::Ok) break;
            if (rmdir(path.c_str()) != 0) { status = StoreStatus::IoError; break; }
        } else if (unlink(path.c_str()) != 0) { status = StoreStatus::IoError; break; }
    }
    if (closedir(entries) != 0) status = StoreStatus::IoError;
    return status;
}
}

namespace store_internal {
bool relative_path(const std::string& path) {
    if (!text_value(path, 1024) || path.front() == '/' ||
        path.find('\\') != std::string::npos || path.find('\0') != std::string::npos) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        auto end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const auto part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return path.back() != '/';
}

StoreStatus safe_path(const std::string& root, const std::string& path) {
    if (!relative_path(path) || root.empty() || root.front() != '/' || root.find('\0') != std::string::npos) return StoreStatus::UnsafePath;
    struct stat info{};
    if (inspect(root.c_str(), &info) != 0) return StoreStatus::IoError;
    if (!S_ISDIR(info.st_mode)) return StoreStatus::UnsafePath;
    std::string current = root;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        current += "/" + path.substr(start, end == std::string::npos ? end : end - start);
        if (inspect(current.c_str(), &info) != 0) return errno == ENOENT ? StoreStatus::Missing : StoreStatus::IoError;
        if (S_ISLNK(info.st_mode)) return StoreStatus::UnsafePath;
        if (end == std::string::npos) return S_ISREG(info.st_mode) || S_ISDIR(info.st_mode) ? StoreStatus::Ok : StoreStatus::UnsafePath;
        if (!S_ISDIR(info.st_mode)) return StoreStatus::UnsafePath;
        start = end + 1;
    }
    return StoreStatus::UnsafePath;
}

StoreStatus read_file(const std::string& root, const std::string& path,
                      std::size_t limit, std::string& contents,
                      const std::function<bool(std::size_t)>& admit) {
    const auto safe = safe_path(root, path);
    if (safe != StoreStatus::Ok) return safe;
    const int fd = open_no_follow(root + "/" + path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? StoreStatus::Missing : StoreStatus::IoError;
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) { close(fd); return StoreStatus::IoError; }
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > limit) { close(fd); return StoreStatus::Invalid; }
    if (admit && !admit(static_cast<std::size_t>(info.st_size))) { close(fd); return StoreStatus::Invalid; }
    // One request per file lets FatFs transfer whole clusters instead of single sectors.
    contents.assign(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t filled = 0;
    while (filled < contents.size()) {
        const auto count = read(fd, contents.data() + filled, contents.size() - filled);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { close(fd); return StoreStatus::IoError; }
        if (count == 0) break;
        filled += static_cast<std::size_t>(count);
    }
    contents.resize(filled);
    return close(fd) == 0 ? StoreStatus::Ok : StoreStatus::IoError;
}

StoreStatus commit_file(const std::string& root, const std::string& path,
                        const std::optional<std::string>& expected, const std::string& bytes,
                        std::size_t limit, std::string& error, bool preserve_backup) {
    error.clear();
    if (!relative_path(path) || path.find('/') == std::string::npos) return StoreStatus::UnsafePath;
    if (bytes.size() > limit || (expected && expected->size() > limit)) return StoreStatus::Invalid;
    const auto failed = [&](const char* step) {
        error = path + ": " + step;
        return StoreStatus::IoError;
    };
    const auto unchanged = [&]() {
        std::string current;
        const auto status = read_file(root, path, limit, current);
        if (status != StoreStatus::Ok && status != StoreStatus::Missing) {
            error = path + ": cannot compare current file";
            return status;
        }
        if ((status == StoreStatus::Ok) != expected.has_value() ||
            (expected && current != *expected)) {
            error = path + ": file changed after it was loaded";
            return StoreStatus::ExternalChange;
        }
        return StoreStatus::Ok;
    };
    auto status = unchanged();
    if (status != StoreStatus::Ok) return status;

    std::size_t previous = std::string::npos;
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        const auto directory = path.substr(0, slash);
        const auto safe = safe_path(root, directory);
        if (safe == StoreStatus::Missing) {
            if (mkdir((root + "/" + directory).c_str(), 0700) != 0 && errno != EEXIST) return failed("directory creation failed");
        } else if (safe != StoreStatus::Ok) {
            error = directory + ": unsafe or unreadable directory";
            return safe;
        }
        struct stat info{};
        if (inspect((root + "/" + directory).c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
            return failed("parent is not a directory");
        }
        const auto parent = previous == std::string::npos ? root : root + "/" + path.substr(0, previous);
        if (!sync_directory(parent)) return failed("directory sync failed");
        previous = slash;
    }
    if (!write_synced(root, path + ".tmp", bytes)) return failed("temporary write or sync failed");
    std::string checked;
    if (read_file(root, path + ".tmp", limit, checked) != StoreStatus::Ok || checked != bytes) {
        return failed("temporary verification failed");
    }
    status = unchanged();
    if (status != StoreStatus::Ok) return status;
    if (expected && !preserve_backup) {
        if (!write_synced(root, path + ".bak.tmp", *expected)) return failed("backup write or sync failed");
        if (read_file(root, path + ".bak.tmp", limit, checked) != StoreStatus::Ok || checked != *expected) {
            return failed("backup verification failed");
        }
        if (!publish(root, path + ".bak.tmp", path + ".bak")) return failed("backup publication failed");
    }
    status = unchanged();
    if (status != StoreStatus::Ok) return status;
    if (!publish(root, path + ".tmp", path)) return failed("publication failed");
    if (read_file(root, path, limit, checked) != StoreStatus::Ok || checked != bytes) {
        return failed("published verification failed");
    }
    return StoreStatus::Ok;
}
}

FileStore::FileStore(std::string root) : root_(std::move(root)) {
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
}

FileImport::FileImport(std::string root, std::string path, std::uint64_t size, std::uint32_t crc, int fd)
    : root_(std::move(root)), path_(std::move(path)), expected_size_(size), expected_crc_(crc), fd_(fd) {}

FileImport::~FileImport() {
    std::string ignored;
    cancel(ignored);
}

StoreStatus FileImport::cancel(std::string& error) {
    error.clear();
    terminal_ = true;
    const bool closed = fd_ < 0 || close(fd_) == 0;
    fd_ = -1;
    if (owns_temporary_) {
        const auto status = remove_import_staging(root_, path_, error);
        if (status != StoreStatus::Ok) return status;
        owns_temporary_ = false;
    }
    if (!closed) { error = "import_close_failed"; return StoreStatus::IoError; }
    return StoreStatus::Ok;
}

std::unique_ptr<FileImport> FileStore::begin_import(const std::string& path, std::uint64_t size,
                                                  std::uint32_t crc32, std::string& error) const {
    error.clear();
    if (!asset_path(path) || !size || size > import_limit ||
        (path.compare(0, 12, "LockScreens/") == 0 && size > lock_screen_limit)) {
        error = "invalid_import_path_or_size"; return {};
    }
    if (store_internal::safe_path(root_, path) != StoreStatus::Missing) { error = "destination_exists_or_unsafe"; return {}; }
    if (!resource_parents(root_, path)) { error = "parent_creation_or_sync_failed"; return {}; }
    const auto temporary = path + ".import.tmp";
    if (remove_import_staging(root_, path, error) != StoreStatus::Ok) return {};
    const int fd = open_no_follow(root_ + "/" + temporary, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0) { error = "staging_create_failed"; return {}; }
    return std::unique_ptr<FileImport>(new FileImport(root_, path, size, crc32, fd));
}

StoreStatus FileImport::append(std::uint64_t offset, const void* data, std::size_t size, std::string& error) {
    error.clear();
    if (terminal_ || fd_ < 0 || !data || !size || size > 2048 || offset != received_ || size > expected_size_ - received_) {
        terminal_ = true; error = "invalid_import_block"; return StoreStatus::Invalid;
    }
    std::size_t done = 0;
    while (done < size) {
        const auto count = write(fd_, static_cast<const char*>(data) + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { terminal_ = true; error = "import_write_failed"; return StoreStatus::IoError; }
        done += static_cast<std::size_t>(count);
    }
    crc_ = resource_crc(crc_, data, size);
    received_ += size;
    return StoreStatus::Ok;
}

StoreStatus FileImport::finish(std::string& error) {
    error.clear();
    if (terminal_ || fd_ < 0 || received_ != expected_size_ || crc_ != expected_crc_) {
        terminal_ = true; error = "import_size_or_crc_mismatch"; return StoreStatus::Invalid;
    }
    terminal_ = true;
    const bool synced = fsync(fd_) == 0;
    const bool closed = close(fd_) == 0;
    fd_ = -1;
    if (!synced || !closed) { error = "import_sync_failed"; return StoreStatus::IoError; }
    std::uint64_t size = 0;
    std::uint32_t crc = 0;
    const auto temporary = path_ + ".import.tmp";
    if (asset_digest(root_, temporary, size, crc) != StoreStatus::Ok || size != expected_size_ || crc != expected_crc_) {
        error = "import_readback_failed"; return StoreStatus::IoError;
    }
    if (store_internal::safe_path(root_, path_) != StoreStatus::Missing) {
        error = "destination_exists_or_unsafe"; return StoreStatus::ExternalChange;
    }
    const auto source = root_ + "/" + temporary;
    const auto destination = root_ + "/" + path_;
#ifdef ESP_PLATFORM
#if defined(CONFIG_FATFS_VFS_RENAME_REPLACES_DESTINATION)
    // A replacement-enabled VFS cannot provide the no-overwrite import contract.
    error = "filesystem_replace_rename_enabled"; return StoreStatus::IoError;
#else
    if (rename(source.c_str(), destination.c_str()) != 0) {
        error = "import_publication_failed"; return StoreStatus::IoError;
    }
#endif
#else
    // link() publishes a complete inode while atomically refusing an existing name.
    if (link(source.c_str(), destination.c_str()) != 0) {
        error = "import_publication_failed"; return StoreStatus::IoError;
    }
    if (unlink(source.c_str()) != 0) { error = "import_staging_cleanup_failed"; return StoreStatus::IoError; }
#endif
    owns_temporary_ = false;
    const auto parent = root_ + "/" + path_.substr(0, path_.rfind('/'));
    if (!sync_directory(parent)) { error = "import_publication_sync_failed"; return StoreStatus::IoError; }
    if (asset_digest(root_, path_, size, crc) != StoreStatus::Ok || size != expected_size_ || crc != expected_crc_) {
        error = "import_published_verification_failed"; return StoreStatus::IoError;
    }
    return StoreStatus::Ok;
}

StoreStatus FileStore::inspect_asset(const std::string& path, std::uint64_t& size,
                                    std::uint32_t& crc32, std::string& error) const {
    error.clear(); size = 0; crc32 = 0;
    if (!asset_path(path)) { error = "invalid_asset_path"; return StoreStatus::UnsafePath; }
    const auto result = asset_digest(root_, path, size, crc32);
    if (result != StoreStatus::Ok) { error = "asset_read_failed"; size = 0; crc32 = 0; }
    return result;
}

StoreStatus FileStore::read_asset_head(const std::string& path, std::uint64_t& size,
                                     std::string& bytes, std::string& error) const {
    size = 0; bytes.clear(); error.clear();
    if (!asset_path(path)) { error = "invalid_asset_path"; return StoreStatus::UnsafePath; }
    const auto safe = store_internal::safe_path(root_, path);
    if (safe != StoreStatus::Ok) { error = "asset_read_failed"; return safe; }
    const int fd = open_no_follow(root_ + "/" + path, O_RDONLY);
    if (fd < 0) { error = "asset_open_failed"; return StoreStatus::IoError; }
    struct stat before{};
    StoreStatus result = StoreStatus::Ok;
    if (fstat(fd, &before) != 0) result = StoreStatus::IoError;
    else if (!S_ISREG(before.st_mode) || before.st_size < 0 ||
             static_cast<std::uint64_t>(before.st_size) > import_limit) result = StoreStatus::Invalid;
    if (result == StoreStatus::Ok) {
        size = static_cast<std::uint64_t>(before.st_size);
        bytes.resize(static_cast<std::size_t>(std::min<std::uint64_t>(size, 256)));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = read(fd, bytes.data() + offset, bytes.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { result = StoreStatus::IoError; break; }
            offset += static_cast<std::size_t>(count);
        }
        struct stat after{};
        if (result == StoreStatus::Ok && (fstat(fd, &after) != 0 || after.st_size != before.st_size))
            result = StoreStatus::ExternalChange;
    }
    if (close(fd) != 0 && result == StoreStatus::Ok) result = StoreStatus::IoError;
    if (result != StoreStatus::Ok) { size = 0; bytes.clear(); error = "asset_read_failed"; }
    return result;
}

ProgressResult FileStore::load_progress(const std::string& book_path, const std::string& book_id,
                                       const std::string& content_revision) const {
    const auto safe = store_internal::safe_path(root_, book_path);
    if (safe != StoreStatus::Ok) return {safe, {}, false, book_path};
    const auto path = book_path + ".picopage/progress.json";
    auto main = read_progress(root_, path, book_id, content_revision);
    if (main.status != StoreStatus::Missing && main.status != StoreStatus::Invalid) return main;
    auto backup = read_progress(root_, path + ".bak", book_id, content_revision);
    if (backup.status == StoreStatus::Ok) {
        backup.recovered_backup = true;
        backup.detail = "recovered committed backup: " + path + ".bak";
        return backup;
    }
    if (backup.status != StoreStatus::Missing) return backup;
    return main;
}

ProgressResult FileStore::save_progress(const std::string& book_path, const ReadingRecord& record) const {
    if (!valid_record(record)) return {StoreStatus::Invalid, {}, false, "invalid reading record"};
    const auto safe = store_internal::safe_path(root_, book_path);
    if (safe != StoreStatus::Ok) return {safe, {}, false, book_path};
    const auto directory = book_path + ".picopage";
    const auto safe_directory = store_internal::safe_path(root_, directory);
    if (safe_directory == StoreStatus::Missing) {
        if (mkdir((root_ + "/" + directory).c_str(), 0700) != 0) return {StoreStatus::IoError, {}, false, directory};
    } else if (safe_directory != StoreStatus::Ok) return {safe_directory, {}, false, directory};
    const auto slash = directory.rfind('/');
    if (!sync_directory(slash == std::string::npos ? root_ : root_ + "/" + directory.substr(0, slash))) {
        return {StoreStatus::IoError, {}, false, "sidecar directory sync failed"};
    }
    const auto path = directory + "/progress.json";
    auto previous = read_progress(root_, path, record.book_id, record.content_revision);
    const bool main_valid = previous.status == StoreStatus::Ok;
    if (main_valid || previous.status == StoreStatus::Cleared) {
        const auto backup = read_progress(root_, path + ".bak", record.book_id, record.content_revision);
        if (backup.status == StoreStatus::UnsupportedVersion || backup.status == StoreStatus::IdentityMismatch) return backup;
    }
    if (previous.status == StoreStatus::Missing || previous.status == StoreStatus::Invalid) {
        auto backup = read_progress(root_, path + ".bak", record.book_id, record.content_revision);
        if (backup.status != StoreStatus::Missing) previous = backup;
    }
    if (previous.status != StoreStatus::Ok && previous.status != StoreStatus::Missing && previous.status != StoreStatus::Invalid && previous.status != StoreStatus::Cleared) return previous;
    if (previous.record && previous.record->writer_id == record.writer_id &&
        previous.record->local_revision >= record.local_revision && !(*previous.record == record)) {
        return {StoreStatus::Invalid, {}, false, "local_revision does not advance within writer scope"};
    }
    const auto bytes = encode(record);
    if (bytes.empty() || bytes.size() > progress_limit) return {StoreStatus::Invalid, {}, false, "record exceeds encoding limits"};
    const auto failure = [&](const std::string& step) { return ProgressResult{StoreStatus::IoError, {}, false, path + ": " + step}; };
    if (!write_synced(root_, path + ".tmp", bytes)) return failure("temporary write or sync failed");
    auto checked = read_progress(root_, path + ".tmp", record.book_id, record.content_revision);
    if (!checked.record || !(*checked.record == record)) return failure("temporary verification failed");
    if (main_valid) {
        const auto backup_bytes = encode(*previous.record);
        if (backup_bytes.empty() || !write_synced(root_, path + ".bak.tmp", backup_bytes)) return failure("backup write or sync failed");
        checked = read_progress(root_, path + ".bak.tmp", record.book_id, record.content_revision);
        if (!checked.record || !(*checked.record == *previous.record)) return failure("backup verification failed");
        if (!publish(root_, path + ".bak.tmp", path + ".bak")) return failure("backup publication failed");
    }
    if (!publish(root_, path + ".tmp", path)) return failure("publication failed");
    checked = read_progress(root_, path, record.book_id, record.content_revision);
    if (!checked.record || !(*checked.record == record)) return failure("published verification failed");
    return checked;
}

BookmarkList FileStore::load_bookmarks(const std::string& book_path, const std::string& book_id,
                                       const std::string& content_revision) const {
    BookmarkList result;
    const auto safe = store_internal::safe_path(root_, book_path);
    if (safe != StoreStatus::Ok) { result.status = safe; result.detail = book_path; return result; }
    const auto path = book_path + ".picopage/bookmarks.json";
    std::optional<std::string> main, backup;
    auto status = read_slot(root_, path, main);
    if (status != StoreStatus::Ok) { result.status = status; result.detail = path; return result; }
    const auto main_digest = digest(main);
    if (main) {
        result = decode_bookmarks(*main, book_id, content_revision);
        if (result.status != StoreStatus::Ok) result.detail = path + ": " + result.detail;
        main.reset();
    }
    status = read_slot(root_, path + ".bak", backup);
    if (status != StoreStatus::Ok) {
        result = BookmarkList{};
        result.status = status; result.detail = path + ".bak";
        return result;
    }
    if (main_digest && backup && (result.status == StoreStatus::Ok || result.status == StoreStatus::IdentityMismatch)) {
        if (decode_bookmarks(*backup, book_id, content_revision).status == StoreStatus::UnsupportedVersion) {
            result = BookmarkList{};
            result.status = StoreStatus::UnsupportedVersion;
            result.detail = path + ".bak: unsupported bookmark schema_version";
        }
    } else if (!main_digest && backup) {
        result = decode_bookmarks(*backup, book_id, content_revision);
        result.recovered_backup = result.status == StoreStatus::Ok;
        result.detail = result.recovered_backup ? "recovered committed backup: " + path + ".bak"
                                                : path + ".bak: " + result.detail;
    }
    result.source = main_digest;
    result.backup_source = digest(backup);
    return result;
}

StoreStatus FileStore::save_bookmarks(const std::string& book_path, const std::string& book_id,
                                      const std::string& content_revision, const BookmarkList& loaded,
                                      const std::vector<Bookmark>& bookmarks, std::uint64_t next_sequence,
                                      BookmarkList& committed) const {
    if (loaded.status != StoreStatus::Ok && loaded.status != StoreStatus::Missing &&
        loaded.status != StoreStatus::IdentityMismatch) return loaded.status;
    if (!text_value(book_id, 256) || !text_value(content_revision, 256) ||
        !valid_bookmarks(bookmarks, next_sequence)) return StoreStatus::Invalid;
    const auto bytes = encode_bookmarks(book_id, content_revision, bookmarks, next_sequence);
    if (bytes.empty() || bytes.size() > kBookmarkFileLimit) return StoreStatus::Invalid;
    const auto safe = store_internal::safe_path(root_, book_path);
    if (safe != StoreStatus::Ok) return safe;
    const auto directory = book_path + ".picopage";
    const auto path = directory + "/bookmarks.json";
    const auto foreign = [&](const std::optional<std::string>& slot) {
        return slot && decode_bookmarks(*slot, book_id, content_revision).status == StoreStatus::IdentityMismatch;
    };
    std::optional<std::string> main;
    bool archive_backup = false;
    {
        std::optional<std::string> backup;
        auto status = read_slot(root_, path + ".bak", backup);
        if (status != StoreStatus::Ok) return status;
        if (digest(backup) != loaded.backup_source) return StoreStatus::ExternalChange;
        archive_backup = loaded.status == StoreStatus::IdentityMismatch && foreign(backup);
    }
    auto status = read_slot(root_, path, main);
    if (status != StoreStatus::Ok) return status;
    if (digest(main) != loaded.source) return StoreStatus::ExternalChange;
    auto expected = std::move(main);
    auto remaining_backup = loaded.backup_source;
    if (loaded.status == StoreStatus::IdentityMismatch) {
        const bool archive_main = foreign(expected);
        const auto stem = directory + "/bookmarks." + archive_prefix(loaded.foreign_book_id) + ".";
        std::string target;
        for (unsigned n = 1; n <= 999 && target.empty(); ++n) {
            const auto candidate = stem + std::to_string(n) + ".json";
            if (store_internal::safe_path(root_, candidate) == StoreStatus::Missing &&
                store_internal::safe_path(root_, candidate + ".bak") == StoreStatus::Missing) target = candidate;
        }
        if (target.empty()) return StoreStatus::IoError;
        if (archive_main) {
            if (!rename_to_new(root_, path, target)) return StoreStatus::IoError;
            expected.reset();
        }
        if (archive_backup) {
            if (!rename_to_new(root_, path + ".bak", archive_main ? target + ".bak" : target)) return StoreStatus::IoError;
            remaining_backup.reset();
        }
    }
    std::string error;
    status = store_internal::commit_file(root_, path, expected, bytes, kBookmarkFileLimit, error);
    if (status != StoreStatus::Ok) return status;
    BookmarkList result;
    result.status = StoreStatus::Ok;
    result.bookmarks = bookmarks;
    result.next_sequence = next_sequence;
    result.source = digest(bytes);
    result.backup_source = expected ? digest(expected) : remaining_backup;
    committed = std::move(result);
    return StoreStatus::Ok;
}

StoreStatus FileStore::book_snapshot(const std::string& path, BookSnapshot& snapshot) const {
    snapshot = {};
    auto lower = path;
    for (auto& c : lower) if (c >= 'A' && c <= 'Z') c += 32;
    if (!((lower.size() > 5 && lower.substr(lower.size() - 5) == ".epub") ||
          (lower.size() > 4 && lower.substr(lower.size() - 4) == ".txt")) ||
        lower.rfind("picopage/", 0) == 0 || lower.find(".picopage/") != std::string::npos)
        return StoreStatus::UnsafePath;
    auto status = asset_digest(root_, path, snapshot.size, snapshot.crc32);
    if (status != StoreStatus::Ok) return status;
    snapshot.path = path;
    const auto progress = path + ".picopage/progress.json";
    for (const auto& slot : {std::make_pair(progress, &snapshot.progress),
                             std::make_pair(progress + ".bak", &snapshot.backup)}) {
        std::string bytes;
        status = store_internal::read_file(root_, slot.first, progress_limit, bytes);
        if (status == StoreStatus::Ok) *slot.second = std::move(bytes);
        else if (status != StoreStatus::Missing) return status;
    }
    return StoreStatus::Ok;
}

StoreStatus FileStore::manage_book(const BookSnapshot& snapshot, bool delete_original,
                                 const std::string& book_id, const std::string& revision) const {
    BookSnapshot current;
    const auto status = book_snapshot(snapshot.path, current);
    if (status != StoreStatus::Ok) return status;
    if (current.size != snapshot.size || current.crc32 != snapshot.crc32 ||
        current.progress != snapshot.progress || current.backup != snapshot.backup)
        return StoreStatus::ExternalChange;
    if (delete_original) {
        if (unlink((root_ + "/" + snapshot.path).c_str()) != 0) return StoreStatus::IoError;
        const auto slash = snapshot.path.rfind('/');
        return sync_directory(slash == std::string::npos ? root_ : root_ + "/" + snapshot.path.substr(0, slash))
            ? StoreStatus::Ok : StoreStatus::IoError;
    }
    if (!text_value(book_id, 256) || !text_value(revision, 256)) return StoreStatus::Invalid;
    for (const auto& source : {snapshot.progress, snapshot.backup}) {
        if (!source) continue;
        const auto parsed = decode(*source, book_id, revision);
        if (parsed.status != StoreStatus::Ok && parsed.status != StoreStatus::Cleared &&
            parsed.status != StoreStatus::Invalid) return parsed.status;
    }
    Json object(cJSON_CreateObject(), cJSON_Delete);
    if (!object || !cJSON_AddNumberToObject(object.get(), "schema_version", 2) ||
        !cJSON_AddStringToObject(object.get(), "state", "cleared") ||
        !cJSON_AddStringToObject(object.get(), "book_id", book_id.c_str()) ||
        !cJSON_AddStringToObject(object.get(), "content_revision", revision.c_str())) return StoreStatus::IoError;
    char* printed = cJSON_PrintUnformatted(object.get());
    if (!printed) return StoreStatus::IoError;
    const std::string bytes(printed);
    cJSON_free(printed);
    const auto path = snapshot.path + ".picopage/progress.json";
    std::string error;
    auto committed = store_internal::commit_file(root_, path + ".bak", snapshot.backup, bytes, progress_limit, error, true);
    if (committed != StoreStatus::Ok) return committed;
    return store_internal::commit_file(root_, path, snapshot.progress, bytes, progress_limit, error, true);
}

StoreStatus FileStore::create_top_level_directory(const std::string& name, int& error_number) const {
    error_number = 0;
    if (name.find('/') != std::string::npos) return StoreStatus::UnsafePath;
    const auto safe = store_internal::safe_path(root_, name);
    if (safe != StoreStatus::Missing) return safe;
    if (mkdir((root_ + "/" + name).c_str(), 0700) != 0) {
        error_number = errno;
        return StoreStatus::IoError;
    }
    return sync_directory(root_) ? StoreStatus::Ok : StoreStatus::IoError;
}

StoreStatus FileStore::clear_cache(const std::function<bool()>& stop) const {
    if (stop && stop()) return StoreStatus::Cancelled;
    const std::string path = "PicoPage/cache";
    const auto safe = store_internal::safe_path(root_, path);
    if (safe == StoreStatus::Missing) return StoreStatus::Ok;
    if (safe != StoreStatus::Ok) return safe;
    return remove_children(root_ + "/" + path, 0, stop);
}

StorageUsage FileStore::storage_usage(const std::function<bool()>& stop) const {
    StorageUsage usage;
    if (stop && stop()) return usage;
    usage.roots = load_settings().settings.library_roots;
    std::uint64_t books = 0;
    bool books_known = true;
    for (const auto& path : usage.roots) {
        if (stop && stop()) return {};
        const auto safe = store_internal::safe_path(root_, path);
        if (safe == StoreStatus::Missing) continue;
        if (safe != StoreStatus::Ok) { books_known = false; continue; }
        const auto bytes = tree_bytes(root_, path, true, stop);
        if (bytes) books += *bytes;
        else books_known = false;
    }
    if (books_known) usage.books = books;
    if (stop && stop()) return {};
    const auto safe = store_internal::safe_path(root_, "PicoPage/cache");
    if (safe == StoreStatus::Missing) usage.cache = 0;
    else if (safe == StoreStatus::Ok) usage.cache = tree_bytes(root_, "PicoPage/cache", false, stop);
    if (stop && stop()) return {};
    std::uint64_t total = 0, available = 0;
    if (card_space(total, available)) usage.available = available;
    return usage;
}

bool FileStore::card_space(std::uint64_t& total, std::uint64_t& available) const {
#ifdef ESP_PLATFORM
    // FatFs loads the FAT32 free-cluster count at mount; f_getfree scans the FAT if the count is invalid.
    return esp_vfs_fat_info(root_.c_str(), &total, &available) == ESP_OK;
#else
    struct statvfs info {};
    if (statvfs(root_.c_str(), &info) != 0) return false;
    total = static_cast<std::uint64_t>(info.f_blocks) * info.f_frsize;
    available = static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
    return true;
#endif
}

StoreStatus FileStore::load_cache(const std::string& name, std::size_t limit, std::string& bytes,
                                  const std::function<bool(std::size_t)>& admit) const {
    return store_internal::read_file(root_, "PicoPage/cache/" + name, limit, bytes, admit);
}

CacheReader::~CacheReader() { close(fd_); }
bool CacheReader::read(char* bytes, std::size_t byte_count) {
    if (byte_count > size_ - offset_ || (byte_count && !bytes)) return false;
    std::size_t filled = 0;
    while (filled < byte_count) {
        const auto count = ::read(fd_, bytes + filled, byte_count - filled);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        filled += static_cast<std::size_t>(count);
        offset_ += static_cast<std::size_t>(count);
    }
    return true;
}
bool CacheReader::rewind() {
    struct stat info{};
    if (fstat(fd_, &info) != 0 || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) != size_ || lseek(fd_, 0, SEEK_SET) != 0) return false;
    offset_ = 0;
    return true;
}
bool CacheReader::read_at(std::size_t byte_offset, char* bytes, std::size_t byte_count) {
    if (byte_offset > size_ || byte_count > size_ - byte_offset ||
        lseek(fd_, static_cast<off_t>(byte_offset), SEEK_SET) != static_cast<off_t>(byte_offset)) return false;
    offset_ = byte_offset;
    return read(bytes, byte_count);
}
bool CacheReader::matches_crc32(std::uint32_t expected, const std::function<bool()>& stop) {
    if (!rewind()) return false;
    char buffer[4096];
    std::uint32_t crc = 0;
    while (offset_ < size_) {
        const auto bytes = std::min(sizeof(buffer), size_ - offset_);
        if ((stop && stop()) || !read(buffer, bytes)) return false;
        crc = resource_crc(crc, buffer, bytes);
    }
    return crc == expected;
}

CacheWriter::~CacheWriter() {
    if (fd_ >= 0) close(fd_);
    if (!committed_) unlink((root_ + "/" + path_ + ".tmp").c_str());
}
bool CacheWriter::write(std::size_t byte_offset, std::string_view bytes) {
    if (failed_ || fd_ < 0 || byte_offset > limit_ || bytes.size() > limit_ - byte_offset ||
        lseek(fd_, static_cast<off_t>(byte_offset), SEEK_SET) != static_cast<off_t>(byte_offset)) {
        failed_ = true; return false;
    }
    for (std::size_t at = 0; at < bytes.size();) {
        const auto count = ::write(fd_, bytes.data() + at, bytes.size() - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { failed_ = true; return false; }
        at += static_cast<std::size_t>(count);
    }
    return true;
}
bool CacheWriter::read(std::size_t byte_offset, char* bytes, std::size_t byte_count) {
    if (failed_ || fd_ < 0 || byte_offset > limit_ || byte_count > limit_ - byte_offset || (byte_count && !bytes) ||
        lseek(fd_, static_cast<off_t>(byte_offset), SEEK_SET) != static_cast<off_t>(byte_offset)) {
        failed_ = true; return false;
    }
    for (std::size_t at = 0; at < byte_count;) {
        const auto count = ::read(fd_, bytes + at, byte_count - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { failed_ = true; return false; }
        at += static_cast<std::size_t>(count);
    }
    return true;
}
bool CacheWriter::sync() {
    if (failed_ || fd_ < 0 || fsync(fd_)) { failed_ = true; return false; }
    return true;
}
bool CacheWriter::commit(std::size_t byte_count) {
    struct stat info{};
    if (failed_ || fd_ < 0 || byte_count > limit_ || fstat(fd_, &info) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) < byte_count ||
        ftruncate(fd_, static_cast<off_t>(byte_count)) || fsync(fd_)) { failed_ = true; return false; }
    const int closed = close(fd_); fd_ = -1;
    if (closed != 0) { failed_ = true; return false; }
    committed_ = publish(root_, path_ + ".tmp", path_);
    return committed_;
}
StoreStatus FileStore::begin_cache(const std::string& name, std::size_t limit,
                                  std::unique_ptr<CacheWriter>& writer) const {
    writer.reset();
    const auto path = "PicoPage/cache/" + name, temporary = path + ".tmp";
    const std::size_t maximum = name.rfind("dictionaries/", 0) == 0 ? 64U * 1024 * 1024 : 2U * 1024 * 1024;
    if (!store_internal::relative_path(path) || limit > maximum) return StoreStatus::Invalid;
    if (!resource_parents(root_, path)) return StoreStatus::IoError;
    const auto safe = store_internal::safe_path(root_, temporary);
    if (safe != StoreStatus::Ok && safe != StoreStatus::Missing) return safe;
    const auto full = root_ + "/" + temporary;
    if (safe == StoreStatus::Ok && unlink(full.c_str())) return StoreStatus::IoError;
    const int fd = open_no_follow(full, O_RDWR | O_CREAT | O_EXCL);
    if (fd < 0) return StoreStatus::IoError;
    writer.reset(new (std::nothrow) CacheWriter(fd, root_, path, limit));
    if (!writer) { close(fd); unlink(full.c_str()); return StoreStatus::IoError; }
    return StoreStatus::Ok;
}
StoreStatus FileStore::open_cache(const std::string& name, std::size_t limit,
                                 std::unique_ptr<CacheReader>& reader) const {
    reader.reset();
    const auto path = "PicoPage/cache/" + name;
    const auto safe = store_internal::safe_path(root_, path);
    if (safe != StoreStatus::Ok) return safe;
    const int fd = open_no_follow(root_ + "/" + path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? StoreStatus::Missing : StoreStatus::IoError;
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) { close(fd); return StoreStatus::IoError; }
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > limit) {
        close(fd); return StoreStatus::Invalid;
    }
    reader.reset(new (std::nothrow) CacheReader(fd, static_cast<std::size_t>(info.st_size)));
    if (!reader) { close(fd); return StoreStatus::IoError; }
    return StoreStatus::Ok;
}

StoreStatus FileStore::save_private(const std::string& name, const std::string& bytes,
                                    std::string& error) const {
    error.clear();
    if (name != "network.toml" && name != "sync.toml") return StoreStatus::UnsafePath;
    const auto path = "PicoPage/" + name;
    std::string current;
    const auto status = store_internal::read_file(root_, path, 8192, current);
    if (status != StoreStatus::Ok && status != StoreStatus::Missing) {
        error = path + ": current file cannot be read";
        return status;
    }
    return store_internal::commit_file(root_, path,
                                       status == StoreStatus::Ok ? std::optional<std::string>(current) : std::nullopt,
                                       bytes, 8192, error, true);
}

StoreStatus FileStore::remove_private(const std::string& name) const {
    if (name != "network.toml" && name != "sync.toml") return StoreStatus::UnsafePath;
    const auto path = "PicoPage/" + name;
    const auto safe = store_internal::safe_path(root_, path);
    if (safe != StoreStatus::Ok) return safe;
    return unlink((root_ + "/" + path).c_str()) == 0 && sync_directory(root_ + "/PicoPage") ? StoreStatus::Ok : StoreStatus::IoError;
}

StoreStatus FileStore::save_cache(const std::string& name, const std::string& bytes,
                                 std::string_view prefix, std::string_view suffix) const {
    const auto path = "PicoPage/cache/" + name;
    if (!store_internal::relative_path(path)) return StoreStatus::UnsafePath;
    return resource_parents(root_, path) && write_synced(root_, path + ".tmp", bytes, prefix, suffix) &&
           publish(root_, path + ".tmp", path) ? StoreStatus::Ok : StoreStatus::IoError;
}

namespace {
constexpr std::size_t recent_limit = 144 * 1024, recent_count = 128;
constexpr const char* recent_path = "PicoPage/data/recent-books.txt";
bool parse_recent(const std::string& bytes, std::vector<RecentBook>& records) {
    if (bytes.compare(0, 10, "PPRECENT1\n") != 0) return false;
    std::size_t start = 10;
    while (start < bytes.size()) {
        const auto end = bytes.find('\n', start), tab = bytes.find('\t', start);
        if (end == std::string::npos || tab == std::string::npos || tab >= end || records.size() >= recent_count) return false;
        std::int64_t utc = 0;
        const auto parsed = std::from_chars(bytes.data() + start, bytes.data() + tab, utc);
        const auto path = bytes.substr(tab + 1, end - tab - 1);
        if (parsed.ec != std::errc() || parsed.ptr != bytes.data() + tab || utc < 0 || utc > 253402300799LL ||
            !store_internal::relative_path(path) || std::any_of(records.begin(), records.end(),
                [&](const auto& item) { return item.path == path; })) return false;
        records.push_back({path, utc});
        start = end + 1;
    }
    return true;
}
}

std::vector<RecentBook> FileStore::load_recent_books(std::string& error) const {
    error.clear();
    for (const auto& path : {std::string(recent_path), std::string(recent_path) + ".bak"}) {
        std::string bytes;
        const auto status = store_internal::read_file(root_, path, recent_limit, bytes);
        if (status == StoreStatus::Missing) continue;
        std::vector<RecentBook> records;
        if (status == StoreStatus::Ok && parse_recent(bytes, records)) {
            error.clear(); return records;
        }
        error = "Recent book record cannot be read";
        if (status != StoreStatus::Ok && status != StoreStatus::Invalid) break;
    }
    return {};
}

bool FileStore::needs_onboarding() const {
    struct stat root_info{};
    if (inspect(root_.c_str(), &root_info) != 0) return errno == ENOENT;
    if (!S_ISDIR(root_info.st_mode)) return false;
    const auto exists_or_unknown = [&](const std::string& path) {
        struct stat info{};
        return inspect((root_ + "/" + path).c_str(), &info) == 0 || errno != ENOENT;
    };
    for (const char* path : {"PicoPage/settings.toml", "PicoPage/settings.toml.bak",
                            "PicoPage/network.toml", "PicoPage/network.toml.bak",
                            "PicoPage/sync.toml", "PicoPage/sync.toml.bak",
                            "PicoPage/data/last-book.txt", "PicoPage/data/last-book.txt.bak",
                            "PicoPage/data/recent-books.txt", "PicoPage/data/recent-books.txt.bak"})
        if (exists_or_unknown(path)) {
            if (std::strcmp(path, "PicoPage/settings.toml") == 0) {
                const auto config = load_settings();
                return config.diagnostics.empty() && config.settings.onboarding_in_progress && !config.settings.onboarding_done;
            }
            return false;
        }
    const auto safety = store_internal::safe_path(root_, "Books");
    if (safety != StoreStatus::Ok && safety != StoreStatus::Missing) return false;
    unsigned remaining = 2000;
    const auto legacy = [&](const auto& self, const std::string& path, unsigned depth) -> bool {
        if (depth >= 6 || path.size() > 512) return true;
        struct stat info{};
        if (inspect((root_ + "/" + path).c_str(), &info) != 0) return errno != ENOENT;
        if (!S_ISDIR(info.st_mode)) return true;
        DIR* dir = opendir((root_ + "/" + path).c_str());
        if (!dir) return true;
        bool found = false;
        while (!found) {
            errno = 0;
            const auto* entry = readdir(dir);
            if (!entry) { found = errno != 0; break; }
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (!remaining) { found = true; break; }
            --remaining;
            const auto child = path + "/" + name;
            if (inspect((root_ + "/" + child).c_str(), &info) != 0) { found = true; break; }
            if (!S_ISDIR(info.st_mode)) continue;
            if (name.size() >= 9 && name.compare(name.size() - 9, 9, ".picopage") == 0) {
                found = exists_or_unknown(child + "/progress.json") ||
                        exists_or_unknown(child + "/progress.json.bak") ||
                        exists_or_unknown(child + "/settings.toml");
            } else if (name.front() != '.') found = self(self, child, depth + 1);
        }
        if (closedir(dir) != 0) found = true;
        return found;
    };
    return !legacy(legacy, "Books", 0);
}

StoreStatus FileStore::save_last_book(const std::string& book_path, std::string& error, std::int64_t utc) const {
    error.clear();
    const auto safe = store_internal::safe_path(root_, book_path);
    if (safe != StoreStatus::Ok) { error = "Last book path is unavailable or unsafe"; return safe; }
    struct stat info{};
    if (inspect((root_ + "/" + book_path).c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        error = "Last book path must refer to an existing file";
        return StoreStatus::Invalid;
    }
    const std::string path = "PicoPage/data/last-book.txt";
    std::string existing;
    const auto status = store_internal::read_file(root_, path, 1026, existing);
    if (status != StoreStatus::Ok && status != StoreStatus::Missing) {
        error = "Last book record cannot be read";
        return status;
    }
    auto previous_path = existing;
    if (!previous_path.empty() && previous_path.back() == '\n') previous_path.pop_back();
    if (!previous_path.empty() && previous_path.back() == '\r') previous_path.pop_back();
    const bool preserve_backup = status == StoreStatus::Ok && !store_internal::relative_path(previous_path);
    const auto committed = store_internal::commit_file(root_, path,
        status == StoreStatus::Ok ? std::optional<std::string>(existing) : std::nullopt,
        book_path + "\n", 1026, error, preserve_backup);
    if (committed != StoreStatus::Ok) return committed;
    auto records = load_recent_books(error);
    if (!error.empty()) return StoreStatus::Invalid;
    records.erase(std::remove_if(records.begin(), records.end(), [&](const auto& item) {
        return item.path == book_path;
    }), records.end());
    if (records.size() == recent_count) records.pop_back();
    records.insert(records.begin(), {book_path, utc >= 1704067200 && utc <= 253402300799LL ? utc : 0});
    std::string bytes = "PPRECENT1\n";
    for (const auto& item : records) bytes += std::to_string(item.opened_at) + "\t" + item.path + "\n";
    std::string previous;
    const auto read = store_internal::read_file(root_, recent_path, recent_limit, previous);
    if (read != StoreStatus::Ok && read != StoreStatus::Missing) { error = "Recent book record cannot be read"; return read; }
    std::vector<RecentBook> checked;
    return store_internal::commit_file(root_, recent_path,
        read == StoreStatus::Ok ? std::optional<std::string>(previous) : std::nullopt,
        bytes, recent_limit, error, read == StoreStatus::Ok && !parse_recent(previous, checked));
}

std::optional<std::string> FileStore::load_last_book(std::string& error) const {
    error.clear();
    const std::string path = "PicoPage/data/last-book.txt";
    for (const auto& candidate : {path, path + ".bak"}) {
        std::string bytes;
        const auto status = store_internal::read_file(root_, candidate, 1026, bytes);
        if (status == StoreStatus::Missing) continue;
        if (status != StoreStatus::Ok && status != StoreStatus::Invalid) {
            error = "Last book record cannot be read safely";
            return {};
        }
        if (!bytes.empty() && bytes.back() == '\n') bytes.pop_back();
        if (!bytes.empty() && bytes.back() == '\r') bytes.pop_back();
        if (status != StoreStatus::Ok || !store_internal::relative_path(bytes)) {
            error = "Last book record contains an invalid path";
            continue;
        }
        const auto safe = store_internal::safe_path(root_, bytes);
        struct stat info{};
        if (safe != StoreStatus::Ok || inspect((root_ + "/" + bytes).c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            error = "The last book is no longer available";
            return {};
        }
        error = candidate == path ? "" : "Recovered the last book from its committed backup";
        return bytes;
    }
    return {};
}

bool valid_lock_screen_name(const std::string& name) {
    if (name.empty() || name.size() > 255 || name.front() == '.' ||
        name.find_first_of("/\\\r\n") != std::string::npos) return false;
    for (unsigned char ch : name) if (ch < 32 || ch == 127) return false;
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    auto ext = name.substr(dot);
    for (auto& ch : ext) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg";
}
StoreStatus FileStore::dictionary_path(const std::string& id, std::string& path) const {
    path.clear();
    if (!valid_dictionary_id(id)) return StoreStatus::UnsafePath;
    const auto relative = "Dictionaries/" + id;
    auto safe = store_internal::safe_path(root_, relative);
    if (safe != StoreStatus::Ok) return safe;
    const bool packed = id.size() >= 7 && id.compare(id.size() - 7, 7, ".ifo.gz") == 0;
    const auto base = relative.substr(0, relative.size() - (packed ? 7 : 4));
    for (const auto* suffix : {".idx", ".idx.gz", ".dict", ".dict.dz", ".syn", ".syn.dz"}) {
        const auto companion = base + suffix;
        safe = store_internal::safe_path(root_, companion);
        if (safe != StoreStatus::Ok && safe != StoreStatus::Missing) return safe;
        if (safe == StoreStatus::Ok) {
            struct stat info{};
            if (inspect((root_ + '/' + companion).c_str(), &info)) return StoreStatus::IoError;
            if (!S_ISREG(info.st_mode)) return StoreStatus::UnsafePath;
        }
    }
    struct stat info{};
    if (inspect((root_ + '/' + relative).c_str(), &info)) return StoreStatus::IoError;
    if (!S_ISREG(info.st_mode)) return StoreStatus::UnsafePath;
    path = root_ + '/' + relative;
    return StoreStatus::Ok;
}

StoreStatus FileStore::dictionary_files(std::vector<std::string>& names, bool& complete,
                                       const std::function<bool()>& stop) const {
    names.clear(); complete = false;
    const auto safe = store_internal::safe_path(root_, "Dictionaries");
    if (safe != StoreStatus::Ok) return safe;
    struct Directory { std::string name; unsigned depth; };
    std::vector<Directory> pending{{"", 0}};
    std::size_t directories = 1, entries = 0;
    complete = true;
    bool limit = false;
    StoreStatus status = StoreStatus::Ok;
    while (!pending.empty() && !limit && status == StoreStatus::Ok) {
        auto current = std::move(pending.back()); pending.pop_back();
        if (stop && stop()) { status = StoreStatus::IoError; break; }
        const auto prefix = current.name.empty() ? std::string() : current.name + '/';
        DIR* dir = opendir((root_ + "/Dictionaries/" + current.name).c_str());
        if (!dir) { status = StoreStatus::IoError; break; }
        for (;;) {
            if (stop && stop()) { status = StoreStatus::IoError; break; }
            errno = 0;
            const auto* entry = readdir(dir);
            if (!entry) { if (errno) status = StoreStatus::IoError; break; }
            if (++entries > 8192) { limit = true; break; }
            const std::string name = entry->d_name;
            if (name.empty() || name.front() == '.' || name == "res") continue;
            const auto id = prefix + name;
            if (id.size() > 512 || !valid_dictionary_id(name + ".ifo")) continue;
            struct stat info{};
            if (inspect((root_ + "/Dictionaries/" + id).c_str(), &info)) { status = StoreStatus::IoError; break; }
            if (S_ISDIR(info.st_mode)) {
                if (current.depth == 4 || directories == 256) { complete = false; continue; }
                pending.push_back({id, current.depth + 1}); ++directories;
            } else if (S_ISREG(info.st_mode) && valid_dictionary_id(id)) {
                if (names.size() == 32) { limit = true; break; }
                names.push_back(id);
            }
        }
        closedir(dir);
    }
    if (status != StoreStatus::Ok) { names.clear(); complete = false; }
    else { complete = complete && !limit; std::sort(names.begin(), names.end()); }
    return status;
}

StoreStatus FileStore::lock_screen_files(std::vector<std::string>& names, const std::function<bool()>& stop) const {
    names.clear();
    const auto safe = store_internal::safe_path(root_, "LockScreens");
    if (safe != StoreStatus::Ok) return safe;
    DIR* dir = opendir((root_ + "/LockScreens").c_str());
    if (!dir) return errno == ENOENT ? StoreStatus::Missing : StoreStatus::IoError;
    StoreStatus status = StoreStatus::Ok;
    std::size_t visited = 0;
    for (;;) {
        if (stop && stop()) { status = StoreStatus::IoError; break; }
        errno = 0;
        const auto* entry = readdir(dir);
        if (!entry) { if (errno) status = StoreStatus::IoError; break; }
        if (++visited > 2048) { status = StoreStatus::Invalid; break; }
        const std::string name = entry->d_name;
        if (!valid_lock_screen_name(name)) continue;
        struct stat info{};
        if (inspect((root_ + "/LockScreens/" + name).c_str(), &info) || !S_ISREG(info.st_mode)) continue;
        if (names.size() == 256) { status = StoreStatus::Invalid; break; }
        names.push_back(name);
    }
    closedir(dir);
    if (status != StoreStatus::Ok) names.clear();
    else std::sort(names.begin(), names.end());
    return status;
}
StoreStatus FileStore::read_lock_screen(const std::string& name, std::vector<std::uint8_t>& bytes,
                                       const std::function<bool()>& stop) const {
    bytes.clear();
    if (!valid_lock_screen_name(name)) return StoreStatus::Invalid;
    const auto path = "LockScreens/" + name;
    const auto safe = store_internal::safe_path(root_, path);
    if (safe != StoreStatus::Ok) return safe;
    struct stat info{};
    if (inspect((root_ + "/" + path).c_str(), &info)) return StoreStatus::Missing;
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 || static_cast<std::uint64_t>(info.st_size) > lock_screen_limit) return StoreStatus::Invalid;
    const auto close_file = [](FILE* file) { fclose(file); };
    std::unique_ptr<FILE, decltype(close_file)> file(fopen((root_ + "/" + path).c_str(), "rb"), close_file);
    if (!file) return StoreStatus::IoError;
    bytes.resize(static_cast<std::size_t>(info.st_size));
    for (std::size_t at = 0; at < bytes.size();) {
        if (stop && stop()) { bytes.clear(); return StoreStatus::IoError; }
        const auto count = std::min<std::size_t>(16384, bytes.size() - at);
        if (fread(bytes.data() + at, 1, count, file.get()) != count) { bytes.clear(); return StoreStatus::IoError; }
        at += count;
    }
    if (fgetc(file.get()) != EOF || ferror(file.get())) { bytes.clear(); return StoreStatus::ExternalChange; }
    return StoreStatus::Ok;
}
StoreStatus FileStore::open_lock_screen(const std::string& name, std::unique_ptr<CacheReader>& reader) const {
    reader.reset();
    if (!valid_lock_screen_name(name)) return StoreStatus::Invalid;
    const auto path = "LockScreens/" + name;
    const auto safe = store_internal::safe_path(root_, path);
    if (safe != StoreStatus::Ok) return safe;
    const int fd = open_no_follow(root_ + "/" + path, O_RDONLY);
    if (fd < 0) return StoreStatus::IoError;
    struct stat info{};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<std::uint64_t>(info.st_size) > lock_screen_limit) { close(fd); return StoreStatus::Invalid; }
    reader.reset(new (std::nothrow) CacheReader(fd, static_cast<std::size_t>(info.st_size)));
    if (!reader) { close(fd); return StoreStatus::IoError; }
    return StoreStatus::Ok;
}
std::string FileStore::last_lock_screen() const {
    for (const auto* path : {"PicoPage/data/last-lock-screen.txt", "PicoPage/data/last-lock-screen.txt.bak"}) {
        std::string value;
        if (store_internal::read_file(root_, path, 256, value) != StoreStatus::Ok) continue;
        if (!value.empty() && value.back() == '\n') value.pop_back();
        if (valid_lock_screen_name(value)) return value;
    }
    return {};
}
StoreStatus FileStore::save_lock_screen(const std::string& name) const {
    if (!valid_lock_screen_name(name)) return StoreStatus::Invalid;
    const std::string path = "PicoPage/data/last-lock-screen.txt";
    std::string previous, error;
    const auto status = store_internal::read_file(root_, path, 256, previous);
    if (status != StoreStatus::Ok && status != StoreStatus::Missing) return status;
    return store_internal::commit_file(root_, path,
        status == StoreStatus::Ok ? std::optional<std::string>(previous) : std::nullopt,
        name + "\n", 256, error);
}

std::int64_t FileStore::last_clock_sync() const {
    for (const auto& path : {std::string(clock_sync_path), std::string(clock_sync_path) + ".bak"}) {
        std::string bytes;
        if (store_internal::read_file(root_, path, clock_sync_limit, bytes) == StoreStatus::Ok) {
            if (const auto seconds = parse_clock_sync(bytes)) return seconds;
        }
    }
    return 0;
}

StoreStatus FileStore::save_clock_sync(std::int64_t utc_seconds) const {
    if (!clock_time::valid(utc_seconds)) return StoreStatus::Invalid;
    std::string previous, error;
    const auto status = store_internal::read_file(root_, clock_sync_path, clock_sync_limit, previous);
    if (status != StoreStatus::Ok && status != StoreStatus::Missing) return status;
    return store_internal::commit_file(root_, clock_sync_path,
        status == StoreStatus::Ok ? std::optional<std::string>(previous) : std::nullopt,
        std::to_string(utc_seconds) + "\n", clock_sync_limit, error, !parse_clock_sync(previous));
}
}
