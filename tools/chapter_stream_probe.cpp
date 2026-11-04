#include "pico_page/document.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <sys/resource.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#endif

using namespace pico_page;
using Clock = std::chrono::steady_clock;

static std::string signature(const Chapter& c) {
    Sha256Digest digest;
    const auto number = [&](std::uint64_t n) {
        char bytes[8];
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<char>(n >> (8 * i));
        if (!digest.update({bytes, sizeof(bytes)})) std::abort();
    };
    const auto text = [&](const std::string& s) {
        number(s.size()); if (!digest.update(s)) std::abort();
    };
    const auto anchor = [&](const Anchor& a) { text(a.resource); text(a.node); number(a.offset); };
    text(c.resource); number(c.part); number(c.last_part); number(c.split_node);
    number(c.plain_dom); number(c.long_source_node); number(c.text.size());
    if (!digest.update({reinterpret_cast<const char*>(c.text.data()), c.text.size() * sizeof(char32_t)})) std::abort();
    number(c.blocks.size());
    for (const auto& b : c.blocks) { number(b.begin); number(b.end); number(static_cast<unsigned>(b.kind)); }
    number(c.runs.size());
    for (const auto& r : c.runs) { number(r.begin); number(r.end); anchor(r.source); number(r.element_reference); }
    number(c.source_nodes.size());
    for (const auto& n : c.source_nodes) {
        text(n.node); number(n.first_child); number(n.text.size());
        if (!digest.update({reinterpret_cast<const char*>(n.text.data()), n.text.size() * sizeof(char32_t)})) std::abort();
    }
    number(c.fragments.size());
    for (const auto& f : c.fragments) { text(f.id); number(f.offset); }
    number(c.links.size());
    for (const auto& l : c.links) { number(l.begin); number(l.end); text(l.resource); text(l.fragment); number(l.external); }
    number(c.images.size());
    for (const auto& i : c.images) { number(i.begin); number(i.end); text(i.resource); text(i.alt); anchor(i.source); }
    number(c.css_sources.size()); for (const auto& s : c.css_sources) text(s);
    number(c.warnings.size()); for (const auto& s : c.warnings) text(s);
    return digest.finish();
}

static std::size_t heap_bytes() {
#ifdef __APPLE__
    malloc_statistics_t stats{};
    malloc_zone_statistics(nullptr, &stats);
    return stats.size_in_use;
#else
    return 0;
#endif
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 5 || (std::string_view(argv[1]) != "load" && std::string_view(argv[1]) != "stream") ||
        (argc == 5 && std::string_view(argv[4]) != "--memory")) {
        std::cerr << "Usage: chapter_stream_probe load|stream book.epub chapter-index|all [--memory]\n";
        return 2;
    }
    const bool sequential = std::string_view(argv[1]) == "stream", memory = argc == 5;
#ifndef __APPLE__
    if (memory) { std::cerr << "Heap sampling requires macOS malloc statistics.\n"; return 2; }
#endif
    EpubDocument document;
    std::string error;
    if (!document.open(argv[2], error)) { std::cerr << error << '\n'; return 1; }
    const bool all = std::string_view(argv[3]) == "all";
    const auto first = all ? 0 : std::stoul(argv[3]);
    const auto end = all ? document.spine().size() : first + 1;
    const auto heap_base = memory ? heap_bytes() : 0;
    const auto zip_base = document.read_stats().zip_allocated_bytes;
    std::cout << "chapter,part,last,scalars,elapsed_us,source_read_bytes,xml_feed_bytes,zip_retained_bytes,heap_retained_bytes,heap_sampled_peak_bytes,checkpoint_max_us,digest\n";
    for (auto index = first; index < end; ++index) {
        for (std::uint32_t part = 0;; ++part) {
            Chapter chapter;
            const auto before = document.read_stats();
            std::size_t peak_heap = memory ? heap_bytes() : 0;
            auto checkpoint = Clock::now();
            std::int64_t checkpoint_max = 0;
            const auto stop = [&] {
                const auto now = Clock::now();
                checkpoint_max = std::max(checkpoint_max, std::chrono::duration_cast<std::chrono::microseconds>(now - checkpoint).count());
                checkpoint = now;
                if (memory) peak_heap = std::max(peak_heap, heap_bytes());
                return false;
            };
            const auto start = Clock::now();
            const bool okay = sequential ? document.read_next_chapter_part(index, chapter, error, stop) :
                document.load_chapter(index, chapter, error, ChapterLoad::Reading, part, stop);
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
            if (!okay) { std::cerr << "chapter=" << index << " part=" << part << " error=" << error << '\n'; return 1; }
            if (memory) peak_heap = std::max(peak_heap, heap_bytes());
            const auto after = document.read_stats();
            const auto digest = signature(chapter);
            const auto scalars = chapter.text.size();
            const bool last = chapter.last_part;
            release(chapter);
            const auto retained_heap = memory ? static_cast<std::int64_t>(heap_bytes()) - static_cast<std::int64_t>(heap_base) : 0;
            std::cout << index << ',' << part << ',' << last << ',' << scalars << ',' << elapsed << ','
                      << after.source_read_bytes - before.source_read_bytes << ',' << after.xml_feed_bytes - before.xml_feed_bytes << ','
                      << after.zip_allocated_bytes - zip_base << ',' << retained_heap << ','
                      << static_cast<std::int64_t>(peak_heap) - static_cast<std::int64_t>(heap_base) << ',' << checkpoint_max << ',' << digest << '\n';
            if (last) break;
        }
    }
    document.close();
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 1;
#ifdef __APPLE__
    std::cerr << "process_peak_rss_bytes=" << usage.ru_maxrss;
#else
    std::cerr << "process_peak_rss_bytes=" << usage.ru_maxrss * 1024;
#endif
    std::cerr << " heap_instrumented=" << memory << " stream_active=" << document.read_stats().stream_active << '\n';
}
