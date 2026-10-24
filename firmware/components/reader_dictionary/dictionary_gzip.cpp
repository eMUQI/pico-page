#include "dictionary_file.h"
#include "pico_page/document.h"
#include "miniz.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace pico_page::dictionary {
namespace {
std::uint16_t le16(const unsigned char* p) { return p[0] | (std::uint16_t(p[1]) << 8); }
std::uint32_t le32(const unsigned char* p) {
    return p[0] | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
}
Result digest_file(File& file, std::string& digest, const Stop& stop) {
    digest.clear();
    Sha256Digest hash;
    std::array<char, 4096> bytes{};
    for (std::uint32_t at = 0; at < file.size();) {
        const auto count = std::min<std::uint32_t>(bytes.size(), file.size() - at);
        auto result = file.read(at, bytes.data(), count, stop);
        if (!result) return result;
        if (!hash.update({bytes.data(), count})) return failure(Status::IoError, "Dictionary digest failed");
        at += count;
    }
    digest = hash.finish();
    return digest.empty() ? failure(Status::IoError, "Dictionary digest failed") : Result{};
}
Result gunzip(const std::string& path, std::size_t limit,
              const std::function<bool(std::string_view)>& emit, const Stop& stop) {
    File file;
    auto result = file.open(path, false, stop); if (!result) return result;
    if (file.size() < 18) return failure(Status::InvalidFormat, "Truncated gzip dictionary file");
    unsigned char header[10]{};
    if (!(result = file.read(0, header, 10, stop))) return result;
    if (header[0] != 31 || header[1] != 139 || header[2] != 8 || (header[3] & 0xe0))
        return failure(Status::InvalidFormat, "Invalid gzip dictionary header");
    std::uint32_t at = 10, end = file.size() - 8;
    if (header[3] & 4) {
        unsigned char length[2]{};
        if (!(result = file.read(at, length, 2, stop))) return result;
        at += 2 + le16(length);
        if (at > end) return failure(Status::InvalidFormat, "Truncated gzip extra field");
    }
    for (unsigned flag : {8U, 16U}) {
        if (!(header[3] & flag)) continue;
        unsigned char c = 1;
        for (unsigned length = 0; c; ++length) {
            if (length >= 4096) return failure(Status::ResourceLimit, "Gzip header string is too long");
            if (at >= end) return failure(Status::InvalidFormat, "Truncated gzip header string");
            if (!(result = file.read(at++, &c, 1, stop))) return result;
        }
    }
    std::array<unsigned char, 4096> input{};
    if (header[3] & 2) {
        if (at > end || end - at < 2) return failure(Status::InvalidFormat, "Truncated gzip header checksum");
        mz_ulong crc = MZ_CRC32_INIT;
        for (std::uint32_t pos = 0; pos < at;) {
            const auto count = std::min<std::uint32_t>(input.size(), at - pos);
            if (!(result = file.read(pos, input.data(), count, stop))) return result;
            crc = mz_crc32(crc, input.data(), count); pos += count;
        }
        unsigned char expected[2]{};
        if (!(result = file.read(at, expected, 2, stop))) return result;
        if ((crc & 65535) != le16(expected)) return failure(Status::InvalidFormat, "Gzip header checksum mismatch");
        at += 2;
    }
    if (at >= end) return failure(Status::InvalidFormat, "Gzip dictionary has no compressed data");
    unsigned char footer[8]{};
    if (!(result = file.read(end, footer, 8, stop))) return result;
    if (le32(footer + 4) > limit) return failure(Status::ResourceLimit, "Expanded dictionary file exceeds its limit");
    if (!memory_room(48 * 1024)) return failure(Status::ResourceLimit, "Insufficient memory to expand dictionary index");
    std::unique_ptr<tinfl_decompressor> state(new (std::nothrow) tinfl_decompressor);
    std::unique_ptr<unsigned char[]> ring(new (std::nothrow) unsigned char[32768]);
    if (!state || !ring) return failure(Status::ResourceLimit, "No memory to expand dictionary index");
    tinfl_init(state.get());
    std::size_t total = 0, ring_at = 0;
    mz_ulong crc = MZ_CRC32_INIT;
    tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;
    while (at < end) {
        const auto count = std::min<std::uint32_t>(input.size(), end - at);
        if (!(result = file.read(at, input.data(), count, stop))) return result;
        std::size_t consumed = 0;
        do {
            if (stop && stop()) return cancelled();
            std::size_t in_size = count - consumed, out_size = 32768 - ring_at;
            status = tinfl_decompress(state.get(), input.data() + consumed, &in_size,
                ring.get(), ring.get() + ring_at, &out_size, at + count < end ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            consumed += in_size;
            if (status < TINFL_STATUS_DONE || (!in_size && !out_size && status != TINFL_STATUS_DONE))
                return failure(Status::InvalidFormat, "Invalid gzip dictionary data");
            if (out_size > limit - total) return failure(Status::ResourceLimit, "Expanded dictionary file exceeds its limit");
            crc = mz_crc32(crc, ring.get() + ring_at, out_size);
            if (out_size && !emit({reinterpret_cast<char*>(ring.get() + ring_at), out_size}))
                return failure(Status::IoError, "Dictionary expansion output failed");
            total += out_size; ring_at = (ring_at + out_size) % 32768;
            if (status == TINFL_STATUS_DONE) {
                if (consumed != count || at + count != end) return failure(Status::Unsupported, "Multiple gzip members or trailing data are unsupported");
                break;
            }
        } while (consumed < count || status == TINFL_STATUS_HAS_MORE_OUTPUT);
        at += count;
    }
    if (status != TINFL_STATUS_DONE || total != le32(footer + 4) || crc != le32(footer))
        return failure(Status::InvalidFormat, "Gzip dictionary checksum or length mismatch");
    return {};
}
Result expand_index(const std::string& path, FileStore& store, const std::string& cache,
                    std::size_t limit, std::string& digest, const Stop& stop) {
    digest.clear();
    std::unique_ptr<CacheWriter> output;
    if (store.begin_cache(cache, limit, output) != StoreStatus::Ok)
        return failure(Status::IoError, "Cannot create dictionary index cache");
    std::size_t size = 0;
    Sha256Digest hash;
    auto result = gunzip(path, limit, [&](std::string_view bytes) {
        if (!hash.update(bytes) || !output->write(size, bytes)) return false;
        size += bytes.size(); return true;
    }, stop);
    if (!result) return result;
    if (!output->sync()) return failure(Status::IoError, "Dictionary index cache synchronization failed");
    const auto expected = hash.finish();
    Sha256Digest verify;
    std::array<char, 4096> bytes{};
    for (std::size_t at = 0; at < size;) {
        if (stop && stop()) return cancelled();
        const auto count = std::min(bytes.size(), size - at);
        if (!output->read(at, bytes.data(), count) || !verify.update({bytes.data(), count}))
            return failure(Status::IoError, "Dictionary index cache readback failed");
        at += count;
    }
    if (expected.empty() || verify.finish() != expected) return failure(Status::IoError, "Dictionary index cache verification failed");
    if (stop && stop()) return cancelled();
    if (!output->commit(size)) return failure(Status::IoError, "Dictionary index cache publication failed");
    digest = expected;
    return {};
}
}  // namespace pico_page::dictionary
