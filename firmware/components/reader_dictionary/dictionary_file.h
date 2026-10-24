#pragma once

#include "pico_page/dictionary.h"
#include <cstdio>
#include "pico_page/store.h"

namespace pico_page::dictionary {

/** Read-only byte ranges in a plain file or a single-member dictzip stream.
 * Decodes one block at a time, retaining only the last block. Partial range
 * validation does not assert the gzip member's whole-stream CRC.
 */
class File {
public:
    File() = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    Result open(const std::string& path, bool dictzip, const Stop& stop);
    Result open_cache(FileStore& store, const std::string& name, std::size_t limit);
    void close();
    Result read(std::uint32_t offset, void* output, std::size_t count, const Stop& stop);
    void discard_decoded() { decoded_.reset(); decoded_block_ = UINT32_MAX; }
    std::uint32_t size() const { return size_; }
private:
    Result raw(std::uint32_t offset, void* output, std::size_t count);
    Result inflate(std::uint32_t block, const Stop& stop);
    std::unique_ptr<CacheReader> cache_;
    std::FILE* file_ = nullptr;
    std::uint32_t physical_size_ = 0, size_ = 0, data_start_ = 0;
    std::uint16_t block_size_ = 0, block_count_ = 0;
    std::unique_ptr<std::uint32_t[]> offsets_;
    std::unique_ptr<unsigned char[]> decoded_;
    std::uint32_t decoded_block_ = UINT32_MAX;
};

/** Decode one gzip member with full CRC/length validation and bounded output. */
Result gunzip(const std::string& path, std::size_t limit,
              const std::function<bool(std::string_view)>& emit, const Stop& stop);
Result expand_index(const std::string& path, FileStore& store, const std::string& cache,
                    std::size_t limit, std::string& digest, const Stop& stop);
Result digest_file(File& file, std::string& digest, const Stop& stop);

bool memory_room(std::size_t bytes);
bool valid_utf8(const std::string& text);
Result failure(Status status, const char* message);
inline Result cancelled() { return failure(Status::Cancelled, "Dictionary operation cancelled"); }

}  // namespace pico_page::dictionary
