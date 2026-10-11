#include "content_cache.h"
#include "pico_page/stage_timer.h"
#include <algorithm>
#include <cstring>
#include <limits>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace pico_page {
namespace {
constexpr std::size_t limit = 2U * 1024 * 1024;
constexpr char version[] = "content-reading-v3";
bool room() {
    if (!chapter_memory_available(limit)) return false;
#ifdef ESP_PLATFORM
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= 4U * 1024 * 1024 &&
           heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= limit;
#else
    return true;
#endif
}
struct Writer {
    std::string bytes;
    bool okay = true;
    void word(std::uint32_t n) {
        if (bytes.size() > limit - 4) { okay = false; return; }
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<char>(n >> (8 * i)));
    }
    void str(const std::string& s) {
        if (s.size() > limit - bytes.size() || limit - bytes.size() - s.size() < 4) { okay = false; return; }
        word(static_cast<std::uint32_t>(s.size())); bytes += s;
    }
    void anchor(const Anchor& a) { str(a.resource); str(a.node); word(a.offset); }
};
struct Reader {
    const std::string& bytes;
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
        const auto n = count(max); std::string s = bytes.substr(at, n); at += n; return s;
    }
    bool flag() { const auto n = word(); if (n > 1) okay = false; return n != 0; }
    Anchor anchor() { Anchor a; a.resource = str(); a.node = str(); a.offset = word(); return a; }
    bool range(std::uint32_t begin, std::uint32_t end, std::size_t size) {
        // A stripped trailing paragraph separator can leave a block end one past text.size().
        return begin <= end && end <= size + 1;
    }
};
std::string key(const EpubDocument& doc, std::size_t index, std::uint32_t part) {
    return std::string("content/") + version + "/" + doc.metadata().book_id + "/" +
           std::to_string(index) + "/" + std::to_string(part);
}
void save(const FileStore& store, const EpubDocument& doc, std::size_t index, const Chapter& c, const std::function<bool()>& stop) {
    if (!room() || (stop && stop())) return;
    const StageTimer timer{"content_cache_write"};
    Writer w;
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
    if (stop && stop()) return;
    if (!w.okay || w.bytes.size() > limit - 32) return;
    w.bytes += md5_hex(w.bytes);
    if (stop && stop()) return;
    if (store.save_cache(key(doc, index, c.part), w.bytes) != StoreStatus::Ok) return;
    if (c.last_part && !(stop && stop())) store.save_cache(key(doc, index, kLastChapterPart), w.bytes);
}
bool load(const FileStore& store, const EpubDocument& doc, std::size_t index, std::uint32_t part, Chapter& c) {
    if (!room() || index >= doc.spine().size()) return false;
    const StageTimer timer{"content_cache_read"};
    std::string bytes;
    if (store.load_cache(key(doc, index, part), limit, bytes) != StoreStatus::Ok || bytes.size() < 32) return false;
    const auto digest = bytes.substr(bytes.size() - 32); bytes.resize(bytes.size() - 32);
    if (md5_hex(bytes) != digest) return false;
    Reader r{bytes};
    if (r.str() != version || r.str() != doc.metadata().book_id) return false;
    Chapter v;
    v.resource = r.str(); v.part = r.word(); v.last_part = r.flag(); v.split_node = r.flag(); v.plain_dom = r.flag();
    if (v.resource != doc.spine()[index].resource ||
        (part == kLastChapterPart ? !v.last_part : v.part != part)) return false;
    auto n = r.count(2U * kChapterPartScalars + 1);
    v.text.reserve(n);
    for (std::uint32_t j = 0; j < n; ++j) {
        const auto cp = r.word();
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        v.text.push_back(cp);
    }
    n = r.count(8192);
    for (std::uint32_t j = 0; j < n; ++j) {
        TextBlock b; b.begin = r.word(); b.end = r.word(); const auto kind = r.word();
        if (!r.range(b.begin, b.end, v.text.size()) || kind > static_cast<unsigned>(BlockKind::Image)) return false;
        b.kind = static_cast<BlockKind>(kind); v.blocks.push_back(b);
    }
    n = r.count(8192);
    for (std::uint32_t j = 0; j < n; ++j) {
        TextRun t; t.begin = r.word(); t.end = r.word(); t.source = r.anchor(); t.element_reference = r.flag();
        if (!r.range(t.begin, t.end, v.text.size()) || t.source.resource != v.resource || t.source.node.empty() ||
            t.source.offset > std::numeric_limits<std::uint32_t>::max() - (t.end - t.begin)) return false;
        v.runs.push_back(std::move(t));
    }
    n = r.count(8192);
    for (std::uint32_t j = 0; j < n; ++j) {
        FragmentTarget f; f.id = r.str(); f.offset = r.word();
        if (f.offset > v.text.size() + 1) return false;
        v.fragments.push_back(std::move(f));
    }
    n = r.count(8192);
    for (std::uint32_t j = 0; j < n; ++j) {
        DocumentLink l; l.begin = r.word(); l.end = r.word(); l.resource = r.str(); l.fragment = r.str(); l.external = r.flag();
        if (!r.range(l.begin, l.end, v.text.size())) return false;
        v.links.push_back(std::move(l));
    }
    n = r.count(256);
    for (std::uint32_t j = 0; j < n; ++j) {
        DocumentImage i; i.begin = r.word(); i.end = r.word(); i.resource = r.str(); i.alt = r.str(); i.source = r.anchor();
        if (!r.range(i.begin, i.end, v.text.size()) || i.source.resource != v.resource) return false;
        v.images.push_back(std::move(i));
    }
    n = r.count(256);
    for (std::uint32_t j = 0; j < n; ++j) v.warnings.push_back(r.str());
    if (!r.okay || r.at != bytes.size() || v.text.empty()) return false;
    c = std::move(v);
    return true;
}
}
bool load_content(const FileStore& store, EpubDocument& doc, std::size_t index, Chapter& chapter,
                  std::string& error, ChapterLoad detail, std::uint32_t part, const std::function<bool()>& stop) {
    release(chapter); error.clear();
    if (stop && stop()) { error = kChapterCancelled; return false; }
    if (detail != ChapterLoad::Mapping && load(store, doc, index, part, chapter)) {
        if (stop && stop()) { release(chapter); error = kChapterCancelled; return false; }
        if (detail == ChapterLoad::Layout) for (auto& run : chapter.runs) { run.source.resource.clear(); run.source.node.clear(); }
        return true;
    }
    return doc.load_chapter(index, chapter, error, detail, part, stop, [&](const Chapter& completed) {
        if (!(stop && stop())) save(store, doc, index, completed, stop);
    });
}
bool find_content(const FileStore& store, EpubDocument& doc, std::size_t index, Chapter& chapter,
                  std::string& error, ChapterLoad detail, const std::function<bool(const Chapter&)>& match, const std::function<bool()>& stop) {
    if (detail == ChapterLoad::Reading) {
        release(chapter); error.clear();
        for (std::uint32_t part = 0; part < 4096; ++part) {
            if (stop && stop()) { release(chapter); error = kChapterCancelled; return false; }
            if (!load(store, doc, index, part, chapter)) break;
            if (match(chapter)) return true;
            if (chapter.last_part) break;
            release(chapter);
        }
    }
    return doc.find_chapter_part(index, chapter, error, detail, match, stop, [&](const Chapter& completed) {
        if (!(stop && stop())) save(store, doc, index, completed, stop);
    });
}
} // namespace pico_page
