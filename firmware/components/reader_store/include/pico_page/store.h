#pragma once

#include "pico_page/record.h"

#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace pico_page {

enum class StoreStatus { Ok, Missing, Invalid, UnsupportedVersion, IdentityMismatch, IoError, UnsafePath, ExternalChange, Cleared };

struct ProgressResult {
    StoreStatus status = StoreStatus::Missing;
    std::optional<ReadingRecord> record;
    bool recovered_backup = false;
    std::string detail;
};

struct BookSnapshot {
    std::string path;
    std::uint64_t size = 0;
    std::uint32_t crc32 = 0;
    std::optional<std::string> progress, backup;
};

struct ReaderSettings {
    std::string font = "Fonts/LXGWWenKai-Regular.ttf";
    int font_size_px = 38;
    double line_height = 1.5;
    int margin_x_px = 36;
    double paragraph_gap_lines = 0;
    int indent_chars = 2;
    std::string refresh_preset = "balanced";
    int balanced_full_refresh_pages = 8;
    int fast_full_refresh_pages = 5;
    std::vector<std::string> library_roots = {"Books"};
    std::string library_view = "list";
    bool wifi_enabled = true;  // Global only; applies to station and access-point sessions.
    int auto_sleep_minutes = 10;  // Global only; 0 disables idle sleep.
    std::string sleep_screen = "cover";  // Global only: cover, reading or simple.
};

struct ConfigDiagnostic {
    StoreStatus status;
    std::string path;
    std::string field;
    std::string detail;
};

struct ConfigResult {
    ReaderSettings settings;
    std::vector<ConfigDiagnostic> diagnostics;
};

struct SettingsSnapshot {
    ConfigResult config;
    ReaderSettings global_settings;
    std::string book_path;
    std::optional<std::string> global_source;
    std::optional<std::string> book_source;
};

/** A single-owner staged resource import; destruction removes its uncommitted temporary file. */
class FileImport {
public:
    ~FileImport();
    FileImport(const FileImport&) = delete;
    FileImport& operator=(const FileImport&) = delete;
    /** Appends the next block, at most 2048 bytes; offsets must be contiguous. */
    StoreStatus append(std::uint64_t offset, const void* data, std::size_t size, std::string& error);
    /** Verifies size/CRC, synchronizes, reads back and publishes without replacing any destination. */
    StoreStatus finish(std::string& error);
    /** Closes the transaction and removes only its unpublished staging file; reports cleanup errors. */
    StoreStatus cancel(std::string& error);
    std::uint64_t received() const { return received_; }
private:
    friend class FileStore;
    FileImport(std::string root, std::string path, std::uint64_t size, std::uint32_t crc, int fd);
    std::string root_, path_;
    std::uint64_t expected_size_, received_ = 0;
    std::uint32_t expected_crc_, crc_ = 0;
    int fd_;
    bool terminal_ = false;
    bool owns_temporary_ = true;
};

/**
 * Serial file access for one mounted card. The caller must stop all operations
 * before unmounting or transferring filesystem ownership to another writer.
 * Paths passed to methods are relative to the card root, without a leading '/'.
 */
class FileStore {
public:
    explicit FileStore(std::string card_root);

    /**
     * Read a committed position matching the caller-verified original content.
     * A valid backup may recover a missing or malformed main file. Temporary
     * files, unsupported schemas and identity mismatches are never adopted.
     */
    ProgressResult load_progress(const std::string& book_path,
                                 const std::string& book_id,
                                 const std::string& content_revision) const;

    /**
     * Commit a successfully presented record and acknowledge its exact value.
     * On any write, sync, verification or publication error, record is empty.
     * A failure after publication may leave a valid record for the next load.
     */
    ProgressResult save_progress(const std::string& book_path,
                                 const ReadingRecord& record) const;

    /**
     * Apply built-in defaults, global settings, then optional book overrides.
     * Each invalid layer is ignored as a whole and reported without rewriting.
     * A missing layer is normal. Font availability is resolved by the renderer.
     */
    ConfigResult load_settings(const std::string& book_path = "") const;

    /** Captures both configuration layers and their effective values for editing. */
    SettingsSnapshot settings_snapshot(const std::string& book_path = "") const;

    /**
     * Applies changed reader/display and global power/network values to one layer after comparing both
     * source snapshots. Invalid source layers or unsafe TOML edits are rejected.
     * Existing comments and untouched fields are preserved. Book changes equal
     * to global values remove the corresponding override. library.roots is
     * read-only through this API; library.view is saved with the global layer only.
     * Refresh the snapshot after every successful save.
     */
    StoreStatus apply_settings(const SettingsSnapshot& snapshot, const ReaderSettings& values,
                               bool book_only, std::string& error) const;

    /** Removes reader/display book overrides, retaining their comments and schema. */
    StoreStatus clear_book_settings(const SettingsSnapshot& snapshot, std::string& error) const;

    /** Saves an existing card-relative book path after successful page presentation. */
    StoreStatus save_last_book(const std::string& book_path, std::string& error) const;

    /** Loads the last existing book, recovering a valid committed backup when needed. */
    std::optional<std::string> load_last_book(std::string& error) const;

    /**
     * Starts a new resource import of 1 byte through 64 MiB. Allowed destinations
     * are EPUB or TXT books in Books/ and fonts or license text in Fonts/.
     * Existing destinations are preserved. An abandoned regular staging file for this
     * destination is removed before starting; unsafe paths are refused.
     * No settings, credentials or reading records are importable.
     * The caller serializes card access until the returned transaction is released.
     */
    std::unique_ptr<FileImport> begin_import(const std::string& path, std::uint64_t size,
                                           std::uint32_t crc32, std::string& error) const;

    /** Returns bounded resource size and CRC32 without exposing file contents. */
    StoreStatus inspect_asset(const std::string& path, std::uint64_t& size,
                              std::uint32_t& crc32, std::string& error) const;

    /** Captures the original book digest and exact progress slots before a management confirmation. */
    StoreStatus book_snapshot(const std::string& path, BookSnapshot& snapshot) const;

    /**
     * Revalidates the snapshot, then deletes only the original book or commits a
     * cleared marker to both progress slots. The caller must first stop the book
     * session and its asynchronous work. Remote records and book settings remain.
     * Failure may leave a published slot; reload before retrying.
     */
    StoreStatus manage_book(const BookSnapshot& snapshot, bool delete_original,
                            const std::string& book_id, const std::string& revision) const;

    /**
     * Replaces PicoPage/network.toml or PicoPage/sync.toml with exact bytes of at most 8 KiB.
     * The previous contents are not kept as a backup because they hold credentials.
     * Any other name is rejected. The caller validates the contents.
     */
    StoreStatus save_private(const std::string& name, const std::string& bytes,
                             std::string& error) const;

    /** Reports the capacity and free space of the mounted card in bytes; false when unavailable. */
    bool card_space(std::uint64_t& total, std::uint64_t& available) const;

    /** Delete only PicoPage/cache children after users of those files stop. */
    StoreStatus clear_cache() const;

    /** Reads a rebuildable file below PicoPage/cache. The caller validates its contents. */
    StoreStatus load_cache(const std::string& name, std::size_t limit, std::string& bytes) const;

    /**
     * Replaces a rebuildable file below PicoPage/cache, creating parent directories.
     * An interrupted call may leave the entry missing; it never holds user data.
     */
    StoreStatus save_cache(const std::string& name, const std::string& bytes) const;

private:
    std::string root_;
};

}  // namespace pico_page
