#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pico_page { class FileStore; }

namespace pico_page::dictionary {

/** Callbacks run on the file-owning worker, at bounded parsing and inflate steps.
 * They may cooperate with the display pause gate. True requests cancellation;
 * borrowed storage remains owned until the enclosing operation returns.
 */
using Stop = std::function<bool()>;
enum class Status { Ok, NotFound, Cancelled, IoError, InvalidFormat, Unsupported, ResourceLimit };
struct Result {
    Status status = Status::Ok;
    std::string message;
    /** A successful read omitted unsupported fields; available text is still usable. */
    bool omitted_fields = false;
    explicit operator bool() const { return status == Status::Ok; }
};
struct Info {
    std::string name;
    std::string version;
    std::string types;
    std::uint32_t words = 0, synonyms = 0, index_bytes = 0;
};
struct Entry {
    std::string headword;
    std::uint32_t ordinal = 0;
    bool synonym = false;
};
struct Field {
    char type = 'm';
    std::string text;
};

/** Reads bounded metadata only; does not prepare or validate its index/data. */
Result inspect(const std::string& ifo_path, Info& info, const Stop& stop = {});

/** Verified source identity for one mounted, exclusively owned card session.
 * The application must clear this on USB handoff, remount, rescan or source
 * replacement. It retains no file handles and cannot outlive that scope.
 */
class Verification {
public:
    void clear() { source_.clear(); fingerprint_.clear(); expanded_digest_.clear(); }
private:
    friend class Dictionary;
    std::string source_, fingerprint_, expanded_digest_;
};

/** Single-owner StarDict reader. All methods perform synchronous file I/O.
 * Keep it on the content worker and destroy it before unmounting storage.
 * open validates the index before queries; no operation writes source files.
 */
class Dictionary {
public:
    Dictionary();
    ~Dictionary();
    Dictionary(const Dictionary&) = delete;
    Dictionary& operator=(const Dictionary&) = delete;
    Result open(const std::string& ifo_path, const Stop& stop = {});
    /** Restores a verified sparse index or prepares it through FileStore.
     * First use in each Verification scope hashes the current source files.
     * Compressed indexes are streamed to cache with CRC and SHA-256 readback.
     */
    Result open_prepared(const std::string& ifo_path, FileStore& store,
                         Verification& verification, const Stop& stop = {});
    bool reused_index() const;
    void close();
    const Info& info() const;
    /** Exact UTF-8 headword lookup, including synonyms. At most 32 distinct
     * entries are returned. Continue with entries.back().ordinal + 1 when more
     * is true; results are ordered by original index ordinal.
     */
    Result lookup(const std::string& word, std::vector<Entry>& entries, bool& more,
                  std::uint32_t first_ordinal = 0, const Stop& stop = {});
    /** Decode textual fields without stripping markup. Binary fields are
     * skipped by their declared lengths. Oversized entries fail explicitly.
     */
    Result read(std::uint32_t ordinal, std::vector<Field>& fields, const Stop& stop = {});
private:
    Result open_impl(const std::string& ifo_path, FileStore* store, Verification* verification, const Stop& stop);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** StarDict ordering: ASCII case-insensitive bytes, then original bytes. */
int compare(const std::string& left, const std::string& right);

}  // namespace pico_page::dictionary
