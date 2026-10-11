#include "pico_page/store.h"
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
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

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

/** Keys of object are distinct, all required ones are present and no others occur. */
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

/** Sorted by strictly increasing sequence, every sequence below next_sequence, within the count bound. */
bool valid_bookmarks(const std::vector<Bookmark>& bookmarks, std::uint64_t next_sequence) {
    if (bookmarks.size() > kMaxBookmarks || next_sequence == 0) return false;
    for (std::size_t i = 0; i < bookmarks.size(); ++i) {
        if (!valid_bookmark(bookmarks[i]) || bookmarks[i].sequence >= next_sequence ||
            (i && bookmarks[i - 1].sequence >= bookmarks[i].sequence)) return false;
    }
    return true;
}

/** True for a JSON \u0000 escape; an escaped backslash followed by literal "u0000" is ordinary text. */
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
    // Indented output keeps the file readable for manual inspection on a computer.
    char* printed = cJSON_Print(object.get());
    if (!printed) return {};
    std::string bytes(printed);
    cJSON_free(printed);
    return bytes;
}

/** Archive name component: up to 8 leading identity characters, restricted to a FAT-safe set. */
std::string archive_prefix(const std::string& book_id) {
    std::string prefix;
    for (char ch : book_id) {
        if (prefix.size() == 8) break;
        const bool safe = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        prefix += safe ? ch : '_';
    }
    return prefix.empty() ? "unknown" : prefix;
}

/** Renames a regular file to a currently missing name; an existing destination is never replaced. */
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

/** Reads one slot; Missing yields nullopt. Any other failure is returned. */
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

bool write_synced(const std::string& root, const std::string& path, const std::string& bytes) {
    const auto safe = store_internal::safe_path(root, path);
    if (safe != StoreStatus::Ok && safe != StoreStatus::Missing) return false;
    const auto full = root + "/" + path;
    // Temporary files are replaced rather than truncated to avoid mutating hard links.
    if (safe == StoreStatus::Ok && unlink(full.c_str()) != 0) return false;
    const int fd = open_no_follow(full, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0) return false;
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count = write(fd, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            close(fd);
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    const bool synced = fsync(fd) == 0;
    return close(fd) == 0 && synced;
}

bool publish(const std::string& root, const std::string& temporary, const std::string& target) {
    const auto safe = store_internal::safe_path(root, target);
    if (safe != StoreStatus::Ok && safe != StoreStatus::Missing) return false;
    const auto full_target = root + "/" + target;
    // FatFs rename refuses an existing destination. The caller retains a valid
    // committed record in the other slot throughout this removal-and-rename gap.
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
    // HTTP imports retain their receive frame while verifying the staged file.
    // Keep the readback buffer off the limited caller task stack.
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

StoreStatus remove_children(const std::string& directory, unsigned depth) {
    if (depth > 32) return StoreStatus::Invalid;
    DIR* entries = opendir(directory.c_str());
    if (!entries) return StoreStatus::IoError;
    StoreStatus status = StoreStatus::Ok;
    while (true) {
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
            status = remove_children(path, depth + 1);
            if (status != StoreStatus::Ok) break;
            if (rmdir(path.c_str()) != 0) { status = StoreStatus::IoError; break; }
        } else if (unlink(path.c_str()) != 0) { status = StoreStatus::IoError; break; }
    }
    if (closedir(entries) != 0) status = StoreStatus::IoError;
    return status;
}
}  // namespace

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
                      std::size_t limit, std::string& contents) {
    const auto safe = safe_path(root, path);
    if (safe != StoreStatus::Ok) return safe;
    const int fd = open_no_follow(root + "/" + path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? StoreStatus::Missing : StoreStatus::IoError;
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) { close(fd); return StoreStatus::IoError; }
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > limit) { close(fd); return StoreStatus::Invalid; }
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
            // A concurrent writer of another sidecar file may create the directory first.
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
}  // namespace store_internal

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
    if (!asset_path(path) || !size || size > import_limit) { error = "invalid_import_path_or_size"; return {}; }
    if (store_internal::safe_path(root_, path) != StoreStatus::Missing) { error = "destination_exists_or_unsafe"; return {}; }
    if (!resource_parents(root_, path)) { error = "parent_creation_or_sync_failed"; return {}; }
    const auto temporary = path + ".import.tmp";
    // Callers serialize the entire import lifetime, so an existing staging file is abandoned.
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
        // A newer firmware's backup would be replaced by the next commit; leave both slots untouched.
        if (decode_bookmarks(*backup, book_id, content_revision).status == StoreStatus::UnsupportedVersion) {
            result = BookmarkList{};
            result.status = StoreStatus::UnsupportedVersion;
            result.detail = path + ".bak: unsupported bookmark schema_version";
        }
    } else if (!main_digest && backup) {
        // The main slot is absent only between backup publication and main publication, or
        // after manual deletion. A backup that cannot be used blocks changes, so it is never
        // superseded before the user inspects it.
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
    // A slot is archived when it does not describe the current content; a backup that
    // matches it, or that cannot be parsed, stays for commit_file to supersede.
    const auto foreign = [&](const std::optional<std::string>& slot) {
        return slot && decode_bookmarks(*slot, book_id, content_revision).status == StoreStatus::IdentityMismatch;
    };
    std::optional<std::string> main;
    bool archive_backup = false;
    // The backup is released before the main slot is read, so only one slot buffer is held.
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
    // commit_file compares the current main slot with these bytes once more before replacing it.
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
    // commit_file keeps the replaced main slot as the backup; without one the old backup remains.
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
    // Publish the cleared backup first so a missing main slot cannot recover an old position.
    auto committed = store_internal::commit_file(root_, path + ".bak", snapshot.backup, bytes, progress_limit, error, true);
    if (committed != StoreStatus::Ok) return committed;
    return store_internal::commit_file(root_, path, snapshot.progress, bytes, progress_limit, error, true);
}

StoreStatus FileStore::clear_cache() const {
    const std::string path = "PicoPage/cache";
    const auto safe = store_internal::safe_path(root_, path);
    if (safe == StoreStatus::Missing) return StoreStatus::Ok;
    if (safe != StoreStatus::Ok) return safe;
    return remove_children(root_ + "/" + path, 0);
}

bool FileStore::card_space(std::uint64_t& total, std::uint64_t& available) const {
#ifdef ESP_PLATFORM
    // The first call after mounting reads the free-cluster count from the FAT32 FSInfo sector;
    // FatFs scans the whole allocation table only when that sector holds no valid count.
    return esp_vfs_fat_info(root_.c_str(), &total, &available) == ESP_OK;
#else
    struct statvfs info {};
    if (statvfs(root_.c_str(), &info) != 0) return false;
    total = static_cast<std::uint64_t>(info.f_blocks) * info.f_frsize;
    available = static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
    return true;
#endif
}

StoreStatus FileStore::load_cache(const std::string& name, std::size_t limit, std::string& bytes) const {
    return store_internal::read_file(root_, "PicoPage/cache/" + name, limit, bytes);
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

StoreStatus FileStore::save_cache(const std::string& name, const std::string& bytes) const {
    const auto path = "PicoPage/cache/" + name;
    if (!store_internal::relative_path(path)) return StoreStatus::UnsafePath;
    return resource_parents(root_, path) && write_synced(root_, path + ".tmp", bytes) &&
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
}  // namespace pico_page
