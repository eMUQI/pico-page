#include "pico_page/store.h"
#include "store_internal.h"

#include "cJSON.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#include "sdkconfig.h"
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
    if (path.compare(0, 6, "Books/") == 0 || path.compare(0, 9, "Articles/") == 0)
        return extension == ".epub";
    if (path.compare(0, 15, "PicoPage/fonts/") == 0)
        return extension == ".ttf" || extension == ".otf" || extension == ".txt";
    if (path.compare(0, 16, "PicoPage/images/") == 0)
        return extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
               extension == ".webp" || extension == ".bmp" || extension == ".pgm";
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
    char buffer[4096];
    while (true) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { close(fd); return StoreStatus::IoError; }
        if (!count) break;
        size += static_cast<std::uint64_t>(count);
        if (size > import_limit) { close(fd); return StoreStatus::Invalid; }
        crc = resource_crc(crc, buffer, static_cast<std::size_t>(count));
    }
    const bool exact_size = size == static_cast<std::uint64_t>(info.st_size);
    return close(fd) == 0 && exact_size ? StoreStatus::Ok : StoreStatus::IoError;
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
    contents.clear();
    char buffer[512];
    while (true) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { close(fd); return StoreStatus::IoError; }
        if (count == 0) break;
        if (contents.size() + static_cast<std::size_t>(count) > limit) { close(fd); return StoreStatus::Invalid; }
        contents.append(buffer, static_cast<std::size_t>(count));
    }
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
            if (mkdir((root + "/" + directory).c_str(), 0700) != 0) return failed("directory creation failed");
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
    if (fd_ >= 0) close(fd_);
    const auto temporary = path_ + ".import.tmp";
    if (owns_temporary_ && store_internal::safe_path(root_, temporary) == StoreStatus::Ok)
        unlink((root_ + "/" + temporary).c_str());
}

std::unique_ptr<FileImport> FileStore::begin_import(const std::string& path, std::uint64_t size,
                                                  std::uint32_t crc32, std::string& error) const {
    error.clear();
    if (!asset_path(path) || !size || size > import_limit) { error = "invalid_import_path_or_size"; return {}; }
    if (store_internal::safe_path(root_, path) != StoreStatus::Missing) { error = "destination_exists_or_unsafe"; return {}; }
    if (!resource_parents(root_, path)) { error = "parent_creation_or_sync_failed"; return {}; }
    const auto temporary = path + ".import.tmp";
    if (store_internal::safe_path(root_, temporary) != StoreStatus::Missing) { error = "staging_exists_or_unsafe"; return {}; }
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
    if (main_valid) {
        const auto backup = read_progress(root_, path + ".bak", record.book_id, record.content_revision);
        if (backup.status == StoreStatus::UnsupportedVersion || backup.status == StoreStatus::IdentityMismatch) return backup;
    }
    if (previous.status == StoreStatus::Missing || previous.status == StoreStatus::Invalid) {
        auto backup = read_progress(root_, path + ".bak", record.book_id, record.content_revision);
        if (backup.status != StoreStatus::Missing) previous = backup;
    }
    if (previous.status != StoreStatus::Ok && previous.status != StoreStatus::Missing && previous.status != StoreStatus::Invalid) return previous;
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

StoreStatus FileStore::clear_cache() const {
    const std::string path = "PicoPage/cache";
    const auto safe = store_internal::safe_path(root_, path);
    if (safe == StoreStatus::Missing) return StoreStatus::Ok;
    if (safe != StoreStatus::Ok) return safe;
    return remove_children(root_ + "/" + path, 0);
}

StoreStatus FileStore::save_last_book(const std::string& book_path, std::string& error) const {
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
    return store_internal::commit_file(root_, path,
        status == StoreStatus::Ok ? std::optional<std::string>(existing) : std::nullopt,
        book_path + "\n", 1026, error, preserve_backup);
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
