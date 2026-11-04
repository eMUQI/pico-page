#include "dictionary_file.h"
#include "miniz.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <new>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace pico_page::dictionary {
namespace {
std::uint16_t le16(const unsigned char* p) { return p[0] | (std::uint16_t(p[1]) << 8); }
std::uint32_t le32(const unsigned char* p) {
    return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
constexpr std::uint32_t kMaxBlocks = 4096;
}
Result failure(Status status, const char* message) { return {status, message}; }

bool memory_room(std::size_t bytes) {
#ifdef ESP_PLATFORM
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= bytes + 768 * 1024 &&
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= bytes;
#else
    static_cast<void>(bytes);
    return true;
#endif
}

bool valid_utf8(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto a = static_cast<unsigned char>(text[i++]);
        if (a < 0x80) { if (!a) return false; continue; }
        unsigned count = a >= 0xc2 && a <= 0xdf ? 1 : a >= 0xe0 && a <= 0xef ? 2 : a >= 0xf0 && a <= 0xf4 ? 3 : 0;
        if (!count || count > text.size() - i) return false;
        std::uint32_t cp = a & ((1U << (6 - count)) - 1);
        for (unsigned n = 0; n < count; ++n) {
            const auto b = static_cast<unsigned char>(text[i++]);
            if ((b & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3f);
        }
        if (cp < (count == 1 ? 0x80U : count == 2 ? 0x800U : 0x10000U) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

File::~File() { close(); }
void File::close() {
    cache_.reset();
    if (file_) std::fclose(file_);
    file_ = nullptr; physical_size_ = size_ = data_start_ = 0;
    block_size_ = block_count_ = 0; decoded_block_ = UINT32_MAX;
    offsets_.reset(); decoded_.reset();
}
Result File::raw(std::uint32_t offset, void* output, std::size_t count) {
    if ((!file_ && !cache_) || offset > physical_size_ || count > physical_size_ - offset)
        return failure(Status::InvalidFormat, "Dictionary range exceeds file size");
    if (cache_) return cache_->read_at(offset, static_cast<char*>(output), count) ? Result{} :
        failure(Status::IoError, "Dictionary cache read failed");
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) ||
        std::fread(output, 1, count, file_) != count)
        return failure(Status::IoError, "Dictionary file read failed");
    return {};
}
Result File::open_cache(FileStore& store, const std::string& name, std::size_t limit) {
    close();
    if (limit > INT32_MAX || store.open_cache(name, limit, cache_) != StoreStatus::Ok)
        return failure(Status::IoError, "Dictionary expanded index cache is unavailable");
    physical_size_ = size_ = static_cast<std::uint32_t>(cache_->size());
    return {};
}
Result File::open(const std::string& path, bool dictzip, const Stop& stop) {
    close();
    if (stop && stop()) return cancelled();
    struct stat st{};
    if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) return failure(Status::IoError, "Dictionary file is missing or unreadable");
    // The target libc uses signed 32-bit seek positions; host builds enforce the same limit.
    if (st.st_size < 0 || std::uint64_t(st.st_size) > INT32_MAX)
        return failure(Status::Unsupported, "Dictionary file exceeds the 32-bit seek limit");
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) return failure(Status::IoError, "Dictionary file open failed");
    physical_size_ = size_ = static_cast<std::uint32_t>(st.st_size);
    if (!dictzip) return {};
    unsigned char header[12];
    auto result = raw(0, header, sizeof(header));
    if (!result) return result;
    if (header[0] != 31 || header[1] != 139 || header[2] != 8 || (header[3] & 0xe0))
        return failure(Status::InvalidFormat, "Invalid dictzip gzip header");
    if (!(header[3] & 4)) return failure(Status::Unsupported, "Gzip dictionary has no random-access table");
    const auto extra_end = std::uint32_t(12) + le16(header + 10);
    if (physical_size_ < 20 || extra_end > physical_size_ - 8)
        return failure(Status::InvalidFormat, "Truncated dictzip extra data");
    std::uint32_t pos = 12;
    while (pos < extra_end) {
        if (stop && stop()) return cancelled();
        unsigned char sub[4];
        if (extra_end - pos < 4) return failure(Status::InvalidFormat, "Truncated dictzip extra field");
        if (!(result = raw(pos, sub, 4))) return result;
        pos += 4;
        auto length = le16(sub + 2);
        if (length > extra_end - pos) return failure(Status::InvalidFormat, "Invalid dictzip extra length");
        if (sub[0] == 'R' && sub[1] == 'A') {
            if (offsets_ || length < 6) return failure(Status::InvalidFormat, "Invalid dictzip random-access table");
            unsigned char ra[6];
            if (!(result = raw(pos, ra, 6))) return result;
            if (le16(ra) != 1) return failure(Status::Unsupported, "Unsupported dictzip table version");
            block_size_ = le16(ra + 2); block_count_ = le16(ra + 4);
            if (!block_size_ || !block_count_ || std::uint32_t(length) != 6U + 2U * block_count_)
                return failure(Status::InvalidFormat, "Invalid dictzip block count or size");
            if (block_count_ > kMaxBlocks) return failure(Status::ResourceLimit, "Dictzip block table exceeds the supported size");
            offsets_.reset(new (std::nothrow) std::uint32_t[std::size_t(block_count_) + 1]);
            if (!offsets_) return failure(Status::ResourceLimit, "No memory for dictzip table");
            offsets_[0] = 0;
            std::array<unsigned char, 512> lengths{};
            for (unsigned first = 0; first < block_count_; first += 256) {
                if (stop && stop()) return cancelled();
                const auto count = std::min<unsigned>(256, block_count_ - first);
                if (!(result = raw(pos + 6 + first * 2, lengths.data(), count * 2))) return result;
                for (unsigned n = 0; n < count; ++n) {
                    const auto i = first + n;
                    const auto compressed = le16(lengths.data() + n * 2);
                    if (!compressed || compressed > physical_size_ - offsets_[i])
                        return failure(Status::InvalidFormat, "Invalid dictzip compressed block length");
                    offsets_[i + 1] = offsets_[i] + compressed;
                }
            }
        }
        pos += length;
    }
    if (!offsets_) return failure(Status::Unsupported, "Gzip dictionary has no random-access table");
    for (const unsigned flag : {8U, 16U}) {
        if (!(header[3] & flag)) continue;
        unsigned n = 0;
        unsigned char ch = 1;
        while (ch) {
            if (++n > 4096) return failure(Status::ResourceLimit, "Dictzip header string is too long");
            if (!(result = raw(pos++, &ch, 1))) return result;
        }
    }
    if (header[3] & 2) return failure(Status::Unsupported, "Dictzip header CRC is not supported");
    data_start_ = pos;
    if (pos > physical_size_ - 8 || offsets_[block_count_] > physical_size_ - 8 - pos)
        return failure(Status::InvalidFormat, "Dictzip blocks exceed the member");
    // dictzip writers may put the final empty DEFLATE block after the RA chunks.
    const auto tail = physical_size_ - 8 - pos - offsets_[block_count_];
    if (tail > 2) return failure(Status::Unsupported, "Unsupported dictzip member layout");
    if (tail) {
        unsigned char end[2]{};
        if (!(result = raw(pos + offsets_[block_count_], end, tail))) return result;
        if (tail != 2 || end[0] != 3 || end[1] != 0)
            return failure(Status::InvalidFormat, "Invalid dictzip final block");
    }
    unsigned char footer[8];
    if (!(result = raw(physical_size_ - 8, footer, 8))) return result;
    size_ = le32(footer + 4);
    if (!size_ || (std::uint64_t(size_) + block_size_ - 1) / block_size_ != block_count_)
        return failure(Status::InvalidFormat, "Dictzip uncompressed size does not match its table");
    return {};
}
Result File::inflate(std::uint32_t block, const Stop& stop) {
    if (decoded_block_ == block) return {};
    decoded_block_ = UINT32_MAX;
    if (!memory_room(std::size_t(block_size_) + 1 + sizeof(tinfl_decompressor)))
        return failure(Status::ResourceLimit, "Insufficient memory for dictzip decompression");
    if (!decoded_) decoded_.reset(new (std::nothrow) unsigned char[std::size_t(block_size_) + 1]);
    std::unique_ptr<tinfl_decompressor> state(new (std::nothrow) tinfl_decompressor);
    if (!decoded_ || !state) return failure(Status::ResourceLimit, "No memory for dictzip decompression");
    tinfl_init(state.get());
    std::array<unsigned char, 4096> input{};
    const auto expected = std::min<std::uint32_t>(block_size_, size_ - block * std::uint32_t(block_size_));
    std::uint32_t cursor = offsets_[block], end = offsets_[block + 1];
    std::size_t produced = 0;
    tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;
    while (cursor < end) {
        if (stop && stop()) return cancelled();
        const auto bytes = std::min<std::uint32_t>(input.size(), end - cursor);
        auto result = raw(data_start_ + cursor, input.data(), bytes);
        if (!result) return result;
        std::size_t consumed = 0;
        do {
            std::size_t in_size = bytes - consumed;
            std::size_t out_size = std::min<std::size_t>(4096, expected + 1 - produced);
            const auto before = produced;
            status = tinfl_decompress(state.get(), input.data() + consumed, &in_size,
                decoded_.get(), decoded_.get() + produced, &out_size,
                TINFL_FLAG_HAS_MORE_INPUT | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            consumed += in_size; produced += out_size;
            if (status < TINFL_STATUS_DONE || produced > expected || (!in_size && produced == before))
                return failure(Status::InvalidFormat, "Invalid dictzip compressed block");
            if (stop && stop()) return cancelled();
            if (status == TINFL_STATUS_DONE) {
                if (consumed != bytes || cursor + bytes != end || block + 1 != block_count_)
                    return failure(Status::InvalidFormat, "Premature dictzip stream end");
                break;
            }
        } while (consumed < bytes || status == TINFL_STATUS_HAS_MORE_OUTPUT);
        cursor += bytes;
    }
    if (produced != expected || (status != TINFL_STATUS_NEEDS_MORE_INPUT && status != TINFL_STATUS_DONE))
        return failure(Status::InvalidFormat, "Dictzip block has the wrong decoded length");
    if (status == TINFL_STATUS_NEEDS_MORE_INPUT) {
        unsigned char flush[4];
        if (end - offsets_[block] < 4) return failure(Status::InvalidFormat, "Missing dictzip flush boundary");
        auto result = raw(data_start_ + end - 4, flush, 4);
        if (!result) return result;
        if (flush[0] || flush[1] || flush[2] != 255 || flush[3] != 255)
            return failure(Status::InvalidFormat, "Invalid dictzip flush boundary");
    }
    decoded_block_ = block;
    return {};
}
Result File::read(std::uint32_t offset, void* output, std::size_t count, const Stop& stop) {
    if (stop && stop()) return cancelled();
    if (offset > size_ || count > size_ - offset) return failure(Status::InvalidFormat, "Dictionary entry exceeds data size");
    if (!offsets_) return raw(offset, output, count);
    auto* dest = static_cast<unsigned char*>(output);
    while (count) {
        const auto block = offset / block_size_;
        auto result = inflate(block, stop);
        if (!result) return result;
        const auto within = offset % block_size_;
        const auto bytes = std::min<std::size_t>(count, block_size_ - within);
        std::memcpy(dest, decoded_.get() + within, bytes);
        dest += bytes; offset += bytes; count -= bytes;
    }
    return {};
}
}
