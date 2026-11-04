#include "pico_page/dictionary.h"
#include "dictionary_file.h"
#include "pico_page/document.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <initializer_list>
#include <new>
#include <sys/stat.h>

namespace pico_page::dictionary {
namespace {
constexpr std::uint32_t kStride = 256;
constexpr std::uint32_t kMaxCheckpoints = 8192;
constexpr std::size_t kMaxWord = 1024;
constexpr std::size_t kMaxEntry = 128 * 1024;
constexpr std::size_t kMaxResults = 32;

bool exists(const std::string& path) { struct stat st{}; return stat(path.c_str(), &st) == 0; }
unsigned fold(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
bool number(const std::string& text, std::uint32_t& out) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

struct Record { std::string word; std::uint32_t offset = 0, length = 0; };
class Cursor {
public:
    Cursor(File& file, std::uint32_t position, const Stop& stop, std::size_t refill_bytes = 4096)
        : file_(file), position_(position), stop_(stop), refill_bytes_(std::min(refill_bytes, buffer_.size())) {}
    std::uint32_t position() const { return position_; }
    Result byte(unsigned char& out) {
        if (position_ >= file_.size()) return failure(Status::InvalidFormat, "Truncated dictionary index record");
        if (available_ == used_) {
            available_ = std::min<std::uint32_t>(refill_bytes_, file_.size() - position_);
            auto result = file_.read(position_, buffer_.data(), available_, stop_);
            if (!result) return result;
            used_ = 0;
        }
        out = buffer_[used_++]; ++position_; return {};
    }
    Result integer(std::uint32_t& out) {
        out = 0;
        for (unsigned i = 0; i < 4; ++i) {
            unsigned char b = 0;
            auto result = byte(b); if (!result) return result;
            out = (out << 8) | b;
        }
        return {};
    }
    Result record(Record& out, bool synonym) {
        if (stop_ && stop_()) return cancelled();
        out.word.clear(); out.offset = out.length = 0;
        for (;;) {
            unsigned char b = 0;
            auto result = byte(b); if (!result) return result;
            if (!b) break;
            if (out.word.size() == kMaxWord) return failure(Status::ResourceLimit, "Dictionary headword is too long");
            out.word += static_cast<char>(b);
        }
        if (out.word.empty() || !valid_utf8(out.word)) return failure(Status::InvalidFormat, "Invalid UTF-8 dictionary headword");
        auto result = integer(out.offset);
        if (!result || synonym) return result;
        return integer(out.length);
    }
private:
    File& file_;
    std::uint32_t position_;
    const Stop& stop_;
    std::array<unsigned char, 4096> buffer_{};
    std::size_t refill_bytes_, used_ = 0, available_ = 0;
};

struct Index {
    File file;
    std::unique_ptr<std::uint32_t[]> checkpoints;
    std::uint32_t count = 0, checkpoint_count = 0;
    bool synonym = false;
    static constexpr std::size_t kMaxCachedCheckpointHeadwords = 128;
    std::vector<std::pair<std::uint32_t, std::string>> checkpoint_headwords;
    std::string first_word, last_word;
    bool first_known = false, last_known = false;
    bool outside(const std::string& word) const {
        return (first_known && compare(word, first_word) < 0) || (last_known && compare(last_word, word) < 0);
    }
    Result prepare(std::uint32_t expected, std::uint32_t data_limit, const Stop& stop) {
        count = expected;
        checkpoint_count = expected / kStride + (expected % kStride != 0);
        if (checkpoint_count > kMaxCheckpoints) return failure(Status::ResourceLimit, "Dictionary sparse index is too large");
        checkpoints.reset(new (std::nothrow) std::uint32_t[checkpoint_count]);
        if (checkpoint_count && !checkpoints) return failure(Status::ResourceLimit, "No memory for dictionary index");
        Cursor cursor(file, 0, stop);
        Record record;
        std::string previous;
        for (std::uint32_t i = 0; i < expected; ++i) {
            if (i % kStride == 0) checkpoints[i / kStride] = cursor.position();
            auto result = cursor.record(record, synonym); if (!result) return result;
            if (i && compare(previous, record.word) > 0) return failure(Status::InvalidFormat, "Dictionary index is not sorted");
            if (synonym ? record.offset >= data_limit : record.offset > data_limit || record.length > data_limit - record.offset)
                return failure(Status::InvalidFormat, "Dictionary index points outside its data");
            previous = record.word;
        }
        if (cursor.position() != file.size()) return failure(Status::InvalidFormat, "Dictionary index count does not match metadata");
        return {};
    }
    Result at(std::uint32_t ordinal, Record& record, const Stop& stop) {
        if (ordinal >= count) return failure(Status::InvalidFormat, "Dictionary entry ordinal is out of range");
        const auto checkpoint = ordinal / kStride;
        Cursor cursor(file, checkpoints[checkpoint], stop);
        for (auto i = checkpoint * kStride; i <= ordinal; ++i) {
            auto result = cursor.record(record, synonym); if (!result) return result;
        }
        return {};
    }
    Result find_lookup_start(const std::string& word, std::uint32_t& offset, std::uint32_t& ordinal, const Stop& stop) {
        std::uint32_t low = 0, high = checkpoint_count;
        Record record;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            const auto memo = std::find_if(checkpoint_headwords.begin(), checkpoint_headwords.end(), [&](const auto& probe) { return probe.first == middle; });
            const std::string* head = memo == checkpoint_headwords.end() ? nullptr : &memo->second;
            if (!head) {
                Cursor cursor(file, checkpoints[middle], stop, 512);
                auto result = cursor.record(record, synonym); if (!result) return result;
                if (checkpoint_headwords.size() < kMaxCachedCheckpointHeadwords) checkpoint_headwords.emplace_back(middle, record.word);
                head = &record.word;
            }
            if (compare(*head, word) < 0) low = middle + 1;
            else high = middle;
        }
        const auto checkpoint = low ? low - 1 : 0;
        offset = checkpoint_count ? checkpoints[checkpoint] : 0;
        ordinal = checkpoint * kStride;
        return {};
    }
};

void append32(std::string& out, std::uint32_t value) {
    for (unsigned shift : {0U, 8U, 16U, 24U}) out += static_cast<char>((value >> shift) & 255);
}
std::uint32_t take32(const std::string& bytes, std::size_t& at) {
    std::uint32_t value = 0;
    for (unsigned shift : {0U, 8U, 16U, 24U}) value |= std::uint32_t(static_cast<unsigned char>(bytes[at++])) << shift;
    return value;
}
std::string source_identity(std::initializer_list<std::string> paths) {
    std::string identity;
    for (const auto& path : paths) {
        if (path.empty()) { identity += "-\n"; continue; }
        struct stat st{};
        if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode)) return {};
        identity += std::to_string(static_cast<long long>(st.st_size)) + ' ' +
                    std::to_string(static_cast<long long>(st.st_mtime)) + '\n';
    }
    return identity;
}
std::string recorded_fingerprint(const FileStore& store, const std::string& name, const std::string& identity) {
    std::string bytes;
    if (store.load_cache(name, 4096, bytes) != StoreStatus::Ok || bytes.size() != 8 + 64 + identity.size() + 64 ||
        bytes.compare(0, 8, "PPDID01\n") || bytes.compare(72, identity.size(), identity)) return {};
    Sha256Digest hash;
    if (!hash.update(std::string_view(bytes).substr(0, bytes.size() - 64)) || hash.finish() != bytes.substr(bytes.size() - 64))
        return {};
    return bytes.substr(8, 64);
}
void record_fingerprint(const FileStore& store, const std::string& name, const std::string& identity,
                        const std::string& fingerprint) {
    auto bytes = "PPDID01\n" + fingerprint + identity;
    Sha256Digest hash;
    if (!hash.update(bytes)) return;
    const auto digest = hash.finish();
    if (!digest.empty()) store.save_cache(name, bytes + digest);
}

std::string serialize_index(const std::string& fingerprint, const std::string& expanded,
                            const Index& index, const Index& synonyms, std::uint32_t data_size) {
    std::string bytes = "PPDIX01\n" + fingerprint + (expanded.empty() ? std::string(64, '0') : expanded);
    for (auto value : {index.count, synonyms.count, index.file.size(), data_size, synonyms.file.size(),
                       index.checkpoint_count, synonyms.checkpoint_count}) append32(bytes, value);
    for (auto* source : {&index, &synonyms})
        for (std::uint32_t i = 0; i < source->checkpoint_count; ++i) append32(bytes, source->checkpoints[i]);
    Sha256Digest hash;
    if (!hash.update(bytes)) return {};
    const auto digest = hash.finish();
    return digest.empty() ? std::string() : bytes + digest;
}
bool restore_index(const std::string& bytes, Index& index, Index& synonyms, std::uint32_t data_size) {
    std::size_t at = 136;
    for (auto value : {index.count, synonyms.count, index.file.size(), data_size, synonyms.file.size()})
        if (take32(bytes, at) != value) return false;
    const auto main_count = take32(bytes, at), syn_count = take32(bytes, at);
    if (main_count > kMaxCheckpoints || syn_count > kMaxCheckpoints ||
        main_count != (index.count + kStride - 1) / kStride ||
        syn_count != (synonyms.count + kStride - 1) / kStride ||
        bytes.size() != at + std::size_t(main_count + syn_count) * 4 + 64) return false;
    for (auto* source : {&index, &synonyms}) {
        source->checkpoint_count = source == &index ? main_count : syn_count;
        source->checkpoints.reset(new (std::nothrow) std::uint32_t[source->checkpoint_count]);
        if (source->checkpoint_count && !source->checkpoints) return false;
        for (std::uint32_t i = 0; i < source->checkpoint_count; ++i) {
            const auto offset = take32(bytes, at);
            if (offset >= source->file.size() || (!i && offset) || (i && offset <= source->checkpoints[i - 1])) return false;
            source->checkpoints[i] = offset;
        }
    }
    return true;
}

Result metadata(const std::string& path, Info& info, const Stop& stop) {
    std::string text;
    if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0) {
        auto result = gunzip(path, 16 * 1024, [&](std::string_view bytes) { text.append(bytes); return true; }, stop);
        if (!result) return result;
    } else {
        File file;
        auto result = file.open(path, false, stop); if (!result) return result;
        if (file.size() > 16 * 1024) return failure(Status::ResourceLimit, "Dictionary metadata exceeds 16 KiB");
        text.resize(file.size());
        if (!(result = file.read(0, text.data(), text.size(), stop))) return result;
    }
    if (!valid_utf8(text)) return failure(Status::InvalidFormat, "Dictionary metadata is not UTF-8");
    std::array<bool, 7> seen{};
    bool magic = false;
    std::size_t position = 0;
    while (position < text.size()) {
        const auto end = text.find('\n', position);
        auto line = text.substr(position, end == std::string::npos ? end : end - position);
        position = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!magic) {
            if (line != "StarDict's dict ifo file") return failure(Status::InvalidFormat, "Invalid StarDict metadata header");
            magic = true; continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        const auto key = line.substr(0, equal), value = line.substr(equal + 1);
        unsigned field = key == "version" ? 0 : key == "bookname" ? 1 : key == "wordcount" ? 2 :
            key == "idxfilesize" ? 3 : key == "synwordcount" ? 4 : key == "sametypesequence" ? 5 : key == "idxoffsetbits" ? 6 : 7;
        if (field == 7) continue;
        if (seen[field]) return failure(Status::InvalidFormat, "Duplicate dictionary metadata key");
        seen[field] = true;
        if (field == 0) info.version = value;
        else if (field == 1) {
            if (value.size() > 512) return failure(Status::ResourceLimit, "Dictionary name exceeds 512 UTF-8 bytes");
            info.name = value;
        }
        else if (field == 2 && !number(value, info.words)) return failure(Status::InvalidFormat, "Invalid dictionary word count");
        else if (field == 3 && !number(value, info.index_bytes)) return failure(Status::InvalidFormat, "Invalid dictionary index size");
        else if (field == 4 && !number(value, info.synonyms)) return failure(Status::InvalidFormat, "Invalid dictionary synonym count");
        else if (field == 5) {
            if (value.size() > 32 || value.empty() || !std::all_of(value.begin(), value.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            })) return failure(Status::InvalidFormat, "Invalid dictionary field sequence");
            info.types = value;
        } else if (field == 6 && value != "32") {
            return failure(value == "64" ? Status::Unsupported : Status::InvalidFormat, "Unsupported dictionary offset width");
        }
    }
    if (!seen[0] || !seen[1] || !seen[2] || !seen[3] || info.name.empty() || !info.words)
        return failure(Status::InvalidFormat, "Dictionary metadata lacks required fields");
    if (info.version != "2.4.2" && info.version != "3.0.0") return failure(Status::Unsupported, "Unsupported StarDict version");
    return {};
}
}

Result inspect(const std::string& ifo_path, Info& info, const Stop& stop) {
    info = {};
    Info parsed;
    if (!memory_room(80 * 1024)) return failure(Status::ResourceLimit, "Insufficient memory for dictionary metadata");
    auto result = metadata(ifo_path, parsed, stop);
    if (result) info = std::move(parsed);
    return result;
}

int compare(const std::string& left, const std::string& right) {
    for (std::size_t i = 0; i < std::min(left.size(), right.size()); ++i) {
        const auto a = fold(static_cast<unsigned char>(left[i])), b = fold(static_cast<unsigned char>(right[i]));
        if (a != b) return a < b ? -1 : 1;
    }
    if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    return left.compare(right);
}

struct Dictionary::Impl {
    Info info;
    Index index, synonyms;
    File data;
    bool ready = false, reused = false;
};
Dictionary::Dictionary() = default;
Dictionary::~Dictionary() = default;
void Dictionary::close() { impl_.reset(); }
const Info& Dictionary::info() const { static const Info empty; return impl_ ? impl_->info : empty; }
bool Dictionary::reused_index() const { return impl_ && impl_->reused; }
Result Dictionary::open(const std::string& ifo_path, const Stop& stop) {
    return open_impl(ifo_path, nullptr, nullptr, stop, true);
}
Result Dictionary::open_prepared(const std::string& ifo_path, FileStore& store,
                                 Verification& verification, const Stop& stop, bool full_check) {
    return open_impl(ifo_path, &store, &verification, stop, full_check);
}
Result Dictionary::open_impl(const std::string& ifo_path, FileStore* store, Verification* verification, const Stop& stop,
                             bool full_check) {
    close();
    std::unique_ptr<Impl> pending(new (std::nothrow) Impl);
    if (!pending) return failure(Status::ResourceLimit, "No memory for dictionary reader");
    const bool packed_ifo = ifo_path.size() > 7 && ifo_path.compare(ifo_path.size() - 7, 7, ".ifo.gz") == 0;
    if (!packed_ifo && (ifo_path.size() < 4 || ifo_path.compare(ifo_path.size() - 4, 4, ".ifo") != 0))
        return failure(Status::Unsupported, "Dictionary metadata must be an .ifo or .ifo.gz file");
    if (!memory_room(192 * 1024)) return failure(Status::ResourceLimit, "Insufficient memory to prepare dictionary");
    auto& state = *pending;
    auto result = metadata(ifo_path, state.info, stop); if (!result) return result;
    if (state.info.words > kMaxCheckpoints * kStride || state.info.synonyms > kMaxCheckpoints * kStride)
        return failure(Status::ResourceLimit, "Dictionary index record count exceeds its limit");
    const auto base = ifo_path.substr(0, ifo_path.size() - (packed_ifo ? 7 : 4));
    const bool compressed = !exists(base + ".dict"), packed_index = !exists(base + ".idx");
    const auto index_path = base + (packed_index ? ".idx.gz" : ".idx");
    const bool syn_compressed = !exists(base + ".syn");
    const auto synonym_path = state.info.synonyms ? base + (syn_compressed ? ".syn.dz" : ".syn") : std::string();
    if (!(result = state.data.open(base + (compressed ? ".dict.dz" : ".dict"), compressed, stop))) return result;
    if (packed_index && !store) return failure(Status::Unsupported, "Compressed dictionary index requires preparation");
    if (state.info.index_bytes > 64 * 1024 * 1024)
        return failure(Status::ResourceLimit, "Dictionary index exceeds 64 MiB");
    std::string fingerprint, cache, expanded_digest, identity;
    bool verified = false;
    if (store) {
        cache = "dictionaries/" + md5_hex(ifo_path) + "/";
        verified = !full_check && verification->source_ == ifo_path && !verification->fingerprint_.empty();
        if (verified) fingerprint = verification->fingerprint_;
        else {
            identity = source_identity({ifo_path, index_path, synonym_path});
            if (!full_check && !identity.empty()) fingerprint = recorded_fingerprint(*store, cache + "identity.bin", identity);
        }
        if (fingerprint.empty()) {
            Sha256Digest combined;
            for (const auto& path : {ifo_path, index_path, synonym_path}) {
                if (path.empty()) continue;
                File source;
                if (!(result = source.open(path, false, stop))) return result;
                std::string digest;
                if (!(result = digest_file(source, digest, stop))) return result;
                if (!combined.update(digest)) return failure(Status::IoError, "Dictionary source digest failed");
            }
            fingerprint = combined.finish();
            if (fingerprint.empty()) return failure(Status::IoError, "Dictionary source digest failed");
            if (!identity.empty()) record_fingerprint(*store, cache + "identity.bin", identity, fingerprint);
        }
    }
    std::string cached;
    if (store) store->load_cache(cache + "index.bin", 80 * 1024, cached);
    bool cache_header = cached.size() >= 228 && cached.compare(0, 8, "PPDIX01\n") == 0 &&
        cached.compare(8, 64, fingerprint) == 0;
    if (cache_header) {
        Sha256Digest hash;
        cache_header = hash.update(std::string_view(cached).substr(0, cached.size() - 64)) &&
            hash.finish() == cached.substr(cached.size() - 64);
    }
    if (packed_index) {
        bool usable = false;
        if (cache_header && state.index.file.open_cache(*store, cache + "index.raw", state.info.index_bytes)) {
            expanded_digest = cached.substr(72, 64);
            if (verified && verification->expanded_digest_ == expanded_digest) usable = true;
            else {
                std::string actual;
                if (!(result = digest_file(state.index.file, actual, stop))) {
                    if (result.status == Status::Cancelled) return result;
                } else usable = actual == expanded_digest;
            }
        }
        if (!usable) {
            state.index.file.close();
            if (!(result = expand_index(index_path, *store, cache + "index.raw", state.info.index_bytes, expanded_digest, stop))) return result;
            if (!(result = state.index.file.open_cache(*store, cache + "index.raw", state.info.index_bytes))) return result;
            cache_header = false;
        }
    } else if (!(result = state.index.file.open(index_path, false, stop))) return result;
    if (state.index.file.size() != state.info.index_bytes) return failure(Status::InvalidFormat, "Dictionary index size does not match metadata");
    if (state.info.synonyms) {
        state.synonyms.synonym = true;
        if (!(result = state.synonyms.file.open(synonym_path, syn_compressed, stop))) return result;
    }
    state.index.count = state.info.words;
    state.synonyms.count = state.info.synonyms;
    if (cache_header) state.reused = restore_index(cached, state.index, state.synonyms, state.data.size());
    std::string().swap(cached);
    if (!state.reused) {
        if (!(result = state.index.prepare(state.info.words, state.data.size(), stop))) return result;
        if (state.info.synonyms && !(result = state.synonyms.prepare(state.info.synonyms, state.info.words, stop))) return result;
        state.synonyms.file.discard_decoded();
        if (store) {
            if (!memory_room(160 * 1024)) return failure(Status::ResourceLimit, "Insufficient memory to publish dictionary index");
            auto payload = serialize_index(fingerprint, expanded_digest, state.index, state.synonyms, state.data.size());
            if (payload.empty()) return failure(Status::IoError, "Dictionary index checksum failed");
            if (stop && stop()) return cancelled();
            std::unique_ptr<CacheWriter> output;
            if (store->begin_cache(cache + "index.bin", 80 * 1024, output) != StoreStatus::Ok ||
                !output->write(0, payload) || !output->sync())
                return failure(Status::IoError, "Dictionary index cache write failed");
            std::array<char, 4096> bytes{};
            for (std::size_t at = 0; at < payload.size();) {
                if (stop && stop()) return cancelled();
                const auto count = std::min(bytes.size(), payload.size() - at);
                if (!output->read(at, bytes.data(), count) || std::memcmp(bytes.data(), payload.data() + at, count))
                    return failure(Status::IoError, "Dictionary index cache readback failed");
                at += count;
            }
            if (stop && stop()) return cancelled();
            if (!output->commit(payload.size())) return failure(Status::IoError, "Dictionary index cache publication failed");
        }
    }
    state.synonyms.file.discard_decoded();
    if (stop && stop()) return cancelled();
    state.ready = true;
    impl_ = std::move(pending);
    if (verification) {
        verification->source_ = ifo_path;
        verification->fingerprint_ = fingerprint;
        verification->expanded_digest_ = expanded_digest;
    }
    return {};
}
Result Dictionary::lookup(const std::string& word, std::vector<Entry>& entries, bool& more,
                          std::uint32_t first_ordinal, const Stop& stop, bool* extensible) {
    entries.clear(); more = false;
    if (extensible) *extensible = false;
    if (!impl_ || !impl_->ready) return failure(Status::InvalidFormat, "Dictionary is not prepared");
    if (word.empty() || word.size() > kMaxWord || !valid_utf8(word)) return failure(Status::InvalidFormat, "Invalid dictionary query");
    if (!memory_room(64 * 1024)) return failure(Status::ResourceLimit, "Insufficient memory for dictionary lookup");
    auto& state = *impl_;
    state.data.discard_decoded();
    const auto extends = [&](const std::string& head) {
        if (head.size() < word.size()) return false;
        for (std::size_t i = 0; i < word.size(); ++i)
            if (fold(static_cast<unsigned char>(head[i])) != fold(static_cast<unsigned char>(word[i]))) return false;
        return true;
    };
    for (auto* source : {&state.index, &state.synonyms}) {
        if (!source->count) continue;
        if (source->outside(word)) {
            if (extensible && source->first_known && compare(word, source->first_word) < 0) *extensible = true;
            continue;
        }
        std::uint32_t offset = 0, ordinal = 0;
        auto result = source->find_lookup_start(word, offset, ordinal, stop); if (!result) { entries.clear(); return result; }
        Cursor cursor(source->file, offset, stop);
        Record record;
        for (; ordinal < source->count; ++ordinal) {
            if (!(result = cursor.record(record, source->synonym))) { entries.clear(); return result; }
            if (!ordinal && !source->first_known) { source->first_word = record.word; source->first_known = true; }
            if (ordinal + 1 == source->count && !source->last_known) { source->last_word = record.word; source->last_known = true; }
            const auto order = compare(record.word, word);
            if (order > 0) { if (extensible && extends(record.word)) *extensible = true; break; }
            if (order < 0) continue;
            const auto target = source->synonym ? record.offset : ordinal;
            if (target < first_ordinal || std::any_of(entries.begin(), entries.end(), [target](const Entry& e) { return e.ordinal == target; })) continue;
            if (entries.size() == kMaxResults && target >= entries.back().ordinal) { more = true; continue; }
            Record actual;
            if (source->synonym) {
                if (!(result = state.index.at(target, actual, stop))) { entries.clear(); return result; }
            }
            Entry entry{source->synonym ? actual.word : record.word, target, source->synonym};
            if (entries.size() == kMaxResults) { entries.pop_back(); more = true; }
            const auto where = std::lower_bound(entries.begin(), entries.end(), target, [](const Entry& e, std::uint32_t n) { return e.ordinal < n; });
            entries.insert(where, std::move(entry));
        }
    }
    if (entries.empty()) return {Status::NotFound, {}};
    return {};
}
Result Dictionary::read(std::uint32_t ordinal, std::vector<Field>& fields, const Stop& stop) {
    fields.clear();
    if (!impl_ || !impl_->ready) return failure(Status::InvalidFormat, "Dictionary is not prepared");
    Record record;
    auto& state = *impl_;
    auto result = state.index.at(ordinal, record, stop); if (!result) return result;
    if (record.length > kMaxEntry) return failure(Status::ResourceLimit, "Dictionary entry exceeds the current decoding limit");
    state.synonyms.file.discard_decoded();
    if (!memory_room(std::size_t(record.length) * 2 + 80 * 1024))
        return failure(Status::ResourceLimit, "Insufficient memory for dictionary definition");
    std::string bytes(record.length, '\0');
    if (!(result = state.data.read(record.offset, bytes.data(), bytes.size(), stop))) return result;
    std::size_t position = 0, sequence = 0;
    std::vector<Field> decoded;
    bool omitted_fields = false;
    unsigned field_count = 0;
    while (position < bytes.size() || sequence < state.info.types.size()) {
        if (stop && stop()) return cancelled();
        if (++field_count > 64) return failure(Status::ResourceLimit, "Dictionary entry has too many fields");
        const bool typed = !state.info.types.empty();
        if (typed && sequence >= state.info.types.size()) return failure(Status::InvalidFormat, "Dictionary entry has excess fields");
        const char type = typed ? state.info.types[sequence++] : bytes[position++];
        const bool binary = type >= 'A' && type <= 'Z';
        if (!binary && !(type >= 'a' && type <= 'z')) return failure(Status::InvalidFormat, "Invalid dictionary field type");
        const bool last = typed && sequence == state.info.types.size();
        std::size_t length = 0;
        if (last) length = bytes.size() - position;
        else if (binary) {
            if (bytes.size() - position < 4) return failure(Status::InvalidFormat, "Truncated dictionary binary field length");
            for (unsigned i = 0; i < 4; ++i) length = (length << 8) | static_cast<unsigned char>(bytes[position++]);
            if (length > bytes.size() - position) return failure(Status::InvalidFormat, "Dictionary binary field exceeds entry");
        } else {
            const auto end = bytes.find('\0', position);
            if (end == std::string::npos) return failure(Status::InvalidFormat, "Unterminated dictionary text field");
            length = end - position;
        }
        if (!binary && (type == 'm' || type == 't' || type == 'h' || type == 'g' || type == 'x')) {
            auto text = bytes.substr(position, length);
            if (!valid_utf8(text)) return failure(Status::InvalidFormat, "Dictionary text field is not valid UTF-8");
            decoded.push_back({type, std::move(text)});
        } else omitted_fields = true;
        position += length;
        if (!last && !binary) ++position;
    }
    if (!state.info.types.empty() && sequence != state.info.types.size()) return failure(Status::InvalidFormat, "Dictionary entry lacks declared fields");
    if (decoded.empty()) return failure(Status::Unsupported, "Dictionary entry has no supported text fields");
    fields = std::move(decoded);
    return {Status::Ok, {}, omitted_fields};
}
}
