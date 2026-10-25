#include "content_cache.h"
#include "pico_page/stage_timer.h"
#include <algorithm>
#include <cstring>
#include <limits>
#if defined(ESP_PLATFORM) && defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

namespace pico_page {
namespace {
constexpr std::size_t limit = 2U * 1024 * 1024;
constexpr char version[] = "content-reading-v3";
bool room(std::size_t bytes) {
    // The document allocator keeps its existing reserve in addition to this allocation.
    const bool admitted = bytes <= limit && chapter_memory_available(bytes + 64);
#if defined(ESP_PLATFORM) && defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
    if (!admitted) ESP_LOGI("chapter_cache", "budget_rejected bytes=%u free=%u largest=%u",
        static_cast<unsigned>(bytes), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#endif
    return admitted;
}
struct Writer {
    std::string bytes;
    std::size_t size = 0;
    bool measuring = true, okay = true;
    void word(std::uint32_t n) {
        if (!okay || size > limit - 4) { okay = false; return; }
        size += 4;
        if (!measuring)
            for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<char>(n >> (8 * i)));
    }
    void str(const std::string& s) {
        if (!okay || s.size() > limit - size || limit - size - s.size() < 4) { okay = false; return; }
        word(static_cast<std::uint32_t>(s.size())); size += s.size();
        if (!measuring) bytes += s;
    }
    void anchor(const Anchor& a) { str(a.resource); str(a.node); word(a.offset); }
};
struct Reader {
    std::string_view bytes;
    std::size_t at = 0;
    bool okay = true;
    std::uint32_t word() {
        if (bytes.size() - at < 4) { okay = false; return 0; }
        std::uint32_t n = 0;
        for (unsigned i = 0; i < 4; ++i) n |= std::uint32_t(static_cast<unsigned char>(bytes[at++])) << (8 * i);
        return n;
    }
    std::uint32_t count(std::size_t max) {
        const auto n = word();
        if (n > max || n > bytes.size() - at) { okay = false; return 0; }
        return n;
    }
    std::string str(std::size_t max = 8192) {
        const auto n = count(max);
        if (!okay || !room(n + 1)) { okay = false; return {}; }
        std::string s(bytes.substr(at, n)); at += n; return s;
    }
    bool flag() { const auto n = word(); if (n > 1) okay = false; return n != 0; }
    Anchor anchor() { Anchor a; a.resource = str(); a.node = str(); a.offset = word(); return a; }
    bool range(std::uint32_t begin, std::uint32_t end, std::size_t size) {
        // A stripped trailing paragraph separator can leave a block end one past text.size().
        return begin <= end && end <= size + 1;
    }
};
std::string key(const std::string& book_id, std::size_t index, std::uint32_t part) {
    return std::string("content/") + version + "/" + book_id + "/" +
           std::to_string(index) + "/" + std::to_string(part);
}
void encode(Writer& w, const EpubDocument& doc, const Chapter& c, const std::function<bool()>& stop) {
    w.str(version); w.str(doc.metadata().book_id); w.str(c.resource);
    w.word(c.part); w.word(c.last_part); w.word(c.split_node); w.word(c.plain_dom);
    w.word(static_cast<std::uint32_t>(c.text.size()));
    for (auto cp : c.text) { if (stop && stop()) return; w.word(cp); }
    w.word(static_cast<std::uint32_t>(c.blocks.size()));
    for (const auto& b : c.blocks) { if (stop && stop()) return; w.word(b.begin); w.word(b.end); w.word(static_cast<unsigned>(b.kind)); }
    w.word(static_cast<std::uint32_t>(c.runs.size()));
    for (const auto& r : c.runs) { if (stop && stop()) return; w.word(r.begin); w.word(r.end); w.anchor(r.source); w.word(r.element_reference); }
    w.word(static_cast<std::uint32_t>(c.fragments.size()));
    for (const auto& f : c.fragments) { if (stop && stop()) return; w.str(f.id); w.word(f.offset); }
    w.word(static_cast<std::uint32_t>(c.links.size()));
    for (const auto& l : c.links) { if (stop && stop()) return; w.word(l.begin); w.word(l.end); w.str(l.resource); w.str(l.fragment); w.word(l.external); }
    w.word(static_cast<std::uint32_t>(c.images.size()));
    for (const auto& i : c.images) { if (stop && stop()) return; w.word(i.begin); w.word(i.end); w.str(i.resource); w.str(i.alt); w.anchor(i.source); }
    w.word(static_cast<std::uint32_t>(c.warnings.size()));
    for (const auto& warning : c.warnings) { if (stop && stop()) return; w.str(warning); }
}
ContentCacheEntry prepare(const EpubDocument& doc, std::size_t index, const Chapter& c,
                          const std::function<bool()>& stop) {
    const StageTimer timer{"content_cache_prepare", 0};
    Writer w;
    encode(w, doc, c, stop);
    if ((stop && stop()) || !w.okay || w.size > limit - 32 || !room(w.size + 33)) return {};
    w.bytes.reserve(w.size + 32);
    w.size = 0; w.measuring = false;
    encode(w, doc, c, stop);
    if ((stop && stop()) || !w.okay) return {};
    w.bytes += md5_hex(w.bytes);
    if (stop && stop()) return {};
    return {key(doc.metadata().book_id, index, c.part), c.last_part ? key(doc.metadata().book_id, index, kLastChapterPart) : "", std::move(w.bytes)};
}
void save(const FileStore& store, const EpubDocument& doc, std::size_t index, const Chapter& c,
          const std::function<bool()>& stop) {
    const auto entry = prepare(doc, index, c, stop);
    save_content_cache(store, entry, stop);
}
bool decode(std::string_view bytes, const std::string& book_id, const std::string& resource,
            std::uint32_t part, Chapter& c, const std::function<bool()>& stop) {
    if (bytes.size() < 32 || bytes.size() > limit || (stop && stop())) return false;
    const auto digest = bytes.substr(bytes.size() - 32); bytes.remove_suffix(32);
    if (md5_hex(bytes) != digest || (stop && stop())) return false;
    Reader r{bytes};
    if (r.str() != version || r.str() != book_id) return false;
    Chapter v;
    v.resource = r.str(); v.part = r.word(); v.last_part = r.flag(); v.split_node = r.flag(); v.plain_dom = r.flag();
    if (v.resource != resource ||
        (part == kLastChapterPart ? !v.last_part : v.part != part)) return false;
    auto n = r.count(2U * kChapterPartScalars + 1);
    if (!r.okay || !room(n * sizeof(char32_t))) return false;
    v.text.reserve(n);
    for (std::uint32_t j = 0; j < n; ++j) {
        if (j % 256 == 0 && stop && stop()) return false;
        const auto cp = r.word();
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        v.text.push_back(cp);
    }
    n = r.count(8192);
    if (!r.okay || !room(n * sizeof(TextBlock))) return false;
    v.blocks.reserve(n);
    for (std::uint32_t j = 0; j < n; ++j) {
        if (j % 256 == 0 && stop && stop()) return false;
        TextBlock b; b.begin = r.word(); b.end = r.word(); const auto kind = r.word();
        if (!r.range(b.begin, b.end, v.text.size()) || kind > static_cast<unsigned>(BlockKind::Image)) return false;
        b.kind = static_cast<BlockKind>(kind); v.blocks.push_back(b);
    }
    n = r.count(8192);
    if (!r.okay || !room(n * sizeof(TextRun))) return false;
    v.runs.reserve(n);
    for (std::uint32_t j = 0; j < n; ++j) {
        if (j % 256 == 0 && stop && stop()) return false;
        TextRun t; t.begin = r.word(); t.end = r.word(); t.source = r.anchor(); t.element_reference = r.flag();
        if (!r.range(t.begin, t.end, v.text.size()) || t.source.resource != v.resource || t.source.node.empty() ||
            t.source.offset > std::numeric_limits<std::uint32_t>::max() - (t.end - t.begin)) return false;
        v.runs.push_back(std::move(t));
    }
    n = r.count(8192);
    if (!r.okay || !room(n * sizeof(FragmentTarget))) return false;
    v.fragments.reserve(n);
    for (std::uint32_t j = 0; j < n; ++j) {
        if (j % 256 == 0 && stop && stop()) return false;
        FragmentTarget f; f.id = r.str(); f.offset = r.word();
        if (f.offset > v.text.size() + 1) return false;
        v.fragments.push_back(std::move(f));
    }
    n = r.count(8192);
    if (!r.okay || !room(n * sizeof(DocumentLink))) return false;
    v.links.reserve(n);
    for (std::uint32_t j = 0; j < n; ++j) {
        if (j % 256 == 0 && stop && stop()) return false;
        DocumentLink l; l.begin = r.word(); l.end = r.word(); l.resource = r.str(); l.fragment = r.str(); l.external = r.flag();
        if (!r.range(l.begin, l.end, v.text.size())) return false;
        v.links.push_back(std::move(l));
    }
    n = r.count(256);
    if (!r.okay || !room(n * sizeof(DocumentImage))) return false;
    v.images.reserve(n);
    for (std::uint32_t j = 0; j < n; ++j) {
        if (j % 256 == 0 && stop && stop()) return false;
        DocumentImage i; i.begin = r.word(); i.end = r.word(); i.resource = r.str(); i.alt = r.str(); i.source = r.anchor();
        if (!r.range(i.begin, i.end, v.text.size()) || i.source.resource != v.resource) return false;
        v.images.push_back(std::move(i));
    }
    n = r.count(256);
    if (!r.okay || !room(n * sizeof(std::string))) return false;
    v.warnings.reserve(n);
    for (std::uint32_t j = 0; j < n; ++j) v.warnings.push_back(r.str());
    if (!r.okay || r.at != bytes.size() || v.text.empty() || (stop && stop())) return false;
    c = std::move(v);
    return true;
}
bool load(const FileStore& store, const EpubDocument& doc, std::size_t index, std::uint32_t part,
          Chapter& chapter, const std::function<bool()>& stop) {
    return index < doc.spine().size() && load_content_cache(store, doc.metadata().book_id,
        doc.spine()[index].resource, index, part, chapter, stop);
}
void save_missing(const FileStore& store, const EpubDocument& doc, std::size_t index,
                  const Chapter& chapter, const std::function<bool()>& stop) {
    std::unique_ptr<CacheReader> reader;
    // Presence only suppresses an optional write; every later cache read validates the payload.
    if (store.open_cache(key(doc.metadata().book_id, index, chapter.part), limit, reader) == StoreStatus::Ok) return;
    save(store, doc, index, chapter, stop);
}
}
bool content_cache_matches(const ContentCacheEntry& entry, const std::string& book_id,
                           std::size_t index, std::uint32_t part) {
    const auto name = key(book_id, index, part);
    return entry.name == name || entry.last_name == name;
}
bool load_content_cache(const FileStore& store, const std::string& book_id, const std::string& resource,
                        std::size_t index, std::uint32_t part, Chapter& chapter,
                        const std::function<bool()>& stop, const ContentCacheEntry* retained) {
    release(chapter);
    if (book_id.empty() || resource.empty() || (stop && stop())) return false;
    const StageTimer timer{"content_cache_read", 0};
    std::string bytes;
    const bool available = retained ? content_cache_matches(*retained, book_id, index, part) :
        store.load_cache(key(book_id, index, part), limit, bytes,
                         [](std::size_t size) { return room(size + 1); }) == StoreStatus::Ok;
    const bool hit = available && decode(retained ? retained->bytes : bytes, book_id, resource, part, chapter, stop);
#if defined(ESP_PLATFORM) && defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
    ESP_LOGI("chapter_cache", "read=%s chapter=%u part=%u retained=%d", hit ? "hit" : "miss",
        static_cast<unsigned>(index), static_cast<unsigned>(part), retained != nullptr);
#endif
    return hit;
}
bool save_content_cache(const FileStore& store, const ContentCacheEntry& entry, const std::function<bool()>& stop) {
    if (entry.bytes.empty() || (stop && stop())) return false;
    const StageTimer timer{"content_cache_write", 0};
    const StageTimer commit_timer{"content_cache_commit", 0};
    const bool saved = store.save_cache(entry.name, entry.bytes) == StoreStatus::Ok &&
        (entry.last_name.empty() || (!(stop && stop()) && store.save_cache(entry.last_name, entry.bytes) == StoreStatus::Ok));
#if defined(ESP_PLATFORM) && defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
    ESP_LOGI("chapter_cache", "write=%s bytes=%u", saved ? "saved" : "failed", static_cast<unsigned>(entry.bytes.size()));
#endif
    return saved;
}
bool load_content(const FileStore& store, EpubDocument& doc, std::size_t index, Chapter& chapter,
                  std::string& error, ChapterLoad detail, std::uint32_t part, const std::function<bool()>& stop, ContentCacheEntry* deferred, bool cache_checked) {
    if (deferred) *deferred = {};
    release(chapter); error.clear();
    if (stop && stop()) { error = kChapterCancelled; return false; }
    if (!cache_checked && detail != ChapterLoad::Mapping && load(store, doc, index, part, chapter, stop)) {
        if (stop && stop()) { release(chapter); error = kChapterCancelled; return false; }
        if (detail == ChapterLoad::Layout) for (auto& run : chapter.runs) { run.source.resource.clear(); run.source.node.clear(); }
        return true;
    }
    const bool loaded = doc.load_chapter(index, chapter, error, detail, part, stop, [&](const Chapter& completed) {
        const bool target = part == kLastChapterPart ? completed.last_part : completed.part == part;
        if (stop && stop()) return;
        if (!deferred) save(store, doc, index, completed, stop);
        else if (!target) save_missing(store, doc, index, completed, stop);
    });
    if (loaded && deferred && detail == ChapterLoad::Reading) *deferred = prepare(doc, index, chapter, stop);
    return loaded;
}
bool find_content(const FileStore& store, EpubDocument& doc, std::size_t index, Chapter& chapter,
                  std::string& error, ChapterLoad detail, const std::function<bool(const Chapter&)>& match, const std::function<bool()>& stop, ContentCacheEntry* deferred) {
    if (deferred) *deferred = {};
    std::uint32_t cached_prefix = 0;
    if (detail == ChapterLoad::Reading) {
        release(chapter); error.clear();
        for (std::uint32_t part = 0; part < 4096; ++part) {
            if (stop && stop()) { release(chapter); error = kChapterCancelled; return false; }
            if (!load(store, doc, index, part, chapter, stop)) break;
            cached_prefix = part + 1;
            if (match(chapter)) return true;
            if (chapter.last_part) break;
            release(chapter);
        }
    }
    const auto select = [&](const Chapter& completed) {
        const bool selected = match(completed);
        // Restore searches need the prefix parts to reach a cached target without reparsing.
        if (deferred && !selected && completed.part >= cached_prefix && detail == ChapterLoad::Reading && !(stop && stop()))
            save(store, doc, index, completed, stop);
        return selected;
    };
    const bool loaded = doc.find_chapter_part(index, chapter, error, detail, select, stop, [&](const Chapter& completed) {
        if (!deferred && !(stop && stop())) save(store, doc, index, completed, stop);
    });
    if (loaded && deferred && detail == ChapterLoad::Reading) *deferred = prepare(doc, index, chapter, stop);
    return loaded;
}
} // namespace pico_page
