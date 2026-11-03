#include "../firmware/components/reader_app/app_impl.h"
#include "host_allocations.h"
#include "miniz.h"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace pico_page;
namespace work = pico_page::host_work;

extern "C" mz_uint __real_mz_zip_reader_get_filename(mz_zip_archive*, mz_uint, char*, mz_uint);
extern "C" mz_uint __wrap_mz_zip_reader_get_filename(mz_zip_archive* zip, mz_uint index, char* out, mz_uint size) {
    work::add(work::ZipNames);
    return __real_mz_zip_reader_get_filename(zip, index, out, size);
}
extern "C" mz_bool __real_mz_zip_reader_extract_to_callback(mz_zip_archive*, mz_uint, mz_file_write_func, void*, mz_uint);
extern "C" mz_bool __wrap_mz_zip_reader_extract_to_callback(mz_zip_archive* zip, mz_uint index,
    mz_file_write_func callback, void* opaque, mz_uint flags) {
    struct Output { mz_file_write_func callback; void* opaque; } output{callback, opaque};
    return __real_mz_zip_reader_extract_to_callback(zip, index, [](void* context, mz_uint64 offset, const void* data, size_t size) {
        auto& output = *static_cast<Output*>(context);
        work::add(work::InflatedBytes, size);
        return output.callback(output.opaque, offset, data, size);
    }, &output, flags);
}

struct Measurement {
    std::array<std::array<std::uint64_t, work::Count>, work::stage_count> counts{};
    std::array<std::uint64_t, work::stage_count> times{};
    host_allocations::Snapshot allocations;
};
template<class Action> Measurement measure(const char* path, unsigned chapters, unsigned sample, Action action) {
    work::reset(); host_allocations::begin(); work::enabled.store(true);
    const auto started = std::chrono::steady_clock::now();
    action();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    work::enabled.store(false);
    Measurement result; result.allocations = host_allocations::end();
    for (unsigned stage = 0; stage < work::stage_count; ++stage) {
        result.times[stage] = stage ? work::durations_us[stage].load() : us;
        for (unsigned counter = 0; counter < work::Count; ++counter)
            result.counts[stage][counter] = work::counters[stage][counter].load();
        if (stage && !result.times[stage]) continue;
        std::cout << path << ',' << chapters << ',' << sample << ',' << work::stages[stage] << ',' << result.times[stage];
        for (const auto count : result.counts[stage]) std::cout << ',' << count;
        // Allocation peaks are capture-wide, not sums of nested stage peaks.
        if (!stage) std::cout << ',' << result.allocations.calls << ',' << result.allocations.bytes << ',' << result.allocations.peak;
        else std::cout << ",,,";
        std::cout << '\n';
    }
    std::cerr << "reader_perf: stage=host_" << path << (chapters == 300 ? "_small" : "_large")
              << " us=" << us << " min_us=0\n";
    return result;
}

namespace pico_page {
/** Drives the private owner operation with a bounded synthetic page index and host pressure. */
class LargeEpubProbe {
public:
    static Measurement reclaim(const fs::path& path, unsigned chapters, unsigned sample, bool pressure) {
        ReaderApp app(path.parent_path().string());
        auto& state = *app.impl_;
        std::string error;
        assert(state.document.open(path.string(), error));
        state.view = View::Reading;
        state.chapter_index = chapters / 2;
        assert(state.document.load_chapter(state.chapter_index, state.chapter, error, ChapterLoad::Reading));
        const auto original_text = state.chapter.text;
        Anchor original_anchor; assert(state.chapter.anchor_at(2, original_anchor));
        state.chapter_pages.resize(chapters);
        state.counted.assign(chapters, true); state.counted_live = state.counted;
        for (auto& parts : state.chapter_pages) parts.emplace_back(1, 0);
        const auto original_bytes = state.page_index_bytes();
        assert(state.page_index_bytes() <= 1024U * 1024);
        work::memory_pressure = pressure;
        const auto result = measure(pressure ? "reclaim_pressure" : "reclaim_idle", chapters, sample, [&] { state.reclaim_caches(); });
        work::memory_pressure = false;
        assert(state.chapter_pages[state.chapter_index].size() == 1);
        assert(state.chapter_pages[state.chapter_index][0] == std::vector<std::uint32_t>{0});
        assert(state.chapter.text == original_text);
        Anchor after; assert(state.chapter.anchor_at(2, after) && after == original_anchor);
        const auto remaining_bytes = state.cache_usage().page_index;
        assert(remaining_bytes == state.page_index_bytes());
        assert(pressure ? remaining_bytes < original_bytes : remaining_bytes == original_bytes);
        for (unsigned i = 0; i < chapters; ++i) if (i != state.chapter_index)
            assert(state.chapter_pages[i].empty() == pressure);
        return result;
    }
};
}

void finish(ContentJob& job) {
    assert(job.start()); job.wait();
    assert(job.done() && job.result.error.empty());
}

int main(int argc, char** argv) {
    assert(argc >= 2 && argc <= 4);
    const fs::path fixtures = argv[1];
    const bool baseline = argc > 2 && std::string_view(argv[2]) == "--measure";
    const auto samples = argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 1U;
    assert(samples > 0 && samples <= 100);
    host_allocations::begin();
    auto* allocation = ::operator new(64, std::align_val_t{64});
    assert(reinterpret_cast<std::uintptr_t>(allocation) % 64 == 0);
    const auto allocated = host_allocations::end();
    assert(allocated.calls == 1 && allocated.bytes == 64 && allocated.peak == 64 && allocated.live == 64);
    ::operator delete(allocation, std::align_val_t{64});
    const auto root = fs::temp_directory_path() / ("pico-large-epub-" + std::to_string(getpid()));
    fs::create_directories(root / "Books");
    std::cout << "path,chapters,sample,stage,us,zip_names,zip_comparisons,spine_comparisons,inflated_bytes,xml_bytes,source_bytes,page_index_visits,reclaim_visits,allocations,allocated_bytes,peak_bytes\n";
    std::array<Measurement, 2> opens, reclaims;
    for (unsigned scale = 0; scale < 2; ++scale) {
        const unsigned chapters = scale ? 3000 : 300;
        const auto path = fixtures / (scale ? "large-toc.epub" : "large-toc-small.epub");
        const auto cover_path = fixtures / ("large-cover-" + std::to_string(chapters) + ".epub");
        for (unsigned sample = 0; sample < samples; ++sample) {
            std::string error;
            EpubDocument document;
            opens[scale] = measure("open", chapters, sample, [&] { assert(document.open(path.string(), error)); });
            assert(document.spine().size() == chapters && document.toc().size() == chapters + chapters / 100);
            Chapter chapter;
            measure("jump_last", chapters, sample, [&] {
                std::size_t index = 0;
                assert(document.chapter_index_for("EPUB/chapter" + std::to_string(chapters - 1) + ".xhtml", index));
                assert(index == chapters - 1 && document.load_chapter(index, chapter, error, ChapterLoad::Reading));
            });
            assert(chapter.text == graphics::decode_utf8("正文 " + std::to_string(chapters - 1)));
            const auto middle = chapters * 3 / 4;
            assert(document.load_chapter(middle, chapter, error, ChapterLoad::Reading));
            Anchor anchor; assert(chapter.anchor_at(2, anchor));
            const std::string relative = "Books/reopen-" + std::to_string(chapters) + "-" + std::to_string(sample) + ".epub";
            fs::copy_file(path, root / relative, fs::copy_options::overwrite_existing);
            FileStore store(root.string());
            const ReadingRecord record{document.metadata().book_id, "epub-native-v1", "probe", 1, anchor};
            assert(store.save_progress(relative, record).status == StoreStatus::Ok);
            ContentJob::Request request;
            request.kind = ContentJob::Kind::Open; request.root = root.string(); request.path = (root / relative).string();
            request.relative_path = relative; request.content_revision = "epub-native-v1";
            ContentJob reopen(std::move(request));
            measure("reopen_middle", chapters, sample, [&] { finish(reopen); });
            assert(reopen.result.chapter_index == middle && reopen.result.offset == 2);
            assert(reopen.result.progress.record && *reopen.result.progress.record == record);
            assert(reopen.result.chapter.text == chapter.text);
            std::vector<std::uint8_t> bytes;
            measure("read_cover", chapters, sample, [&] { assert(EpubDocument::read_cover(cover_path.string(), bytes, error)); });
            assert(!bytes.empty());
            ContentFailureKind missing;
            measure("missing_cover", chapters, sample, [&] {
                assert(!EpubDocument::read_cover(path.string(), bytes, error, {}, &missing));
            });
            assert(missing == ContentFailureKind::MissingResource);
            ContentJob::Request covers; covers.kind = ContentJob::Kind::LibraryCovers; covers.root = root.string();
            for (unsigned i = 0; i < 4; ++i) {
                const auto name = "Books/cover-" + std::to_string(i) + ".epub";
                fs::copy_file(cover_path, root / name, fs::copy_options::overwrite_existing);
                struct stat info{}; assert(stat((root / name).c_str(), &info) == 0);
                covers.covers.push_back({name, "covers/probe-" + std::to_string(i),
                    "PPCOVER4\n" + name + "\n" + std::to_string(info.st_size) + " " + std::to_string(info.st_mtime) + "\n",
                    info.st_size, info.st_mtime});
            }
            fs::remove_all(root / "PicoPage/cache");
            ContentJob cover_job(std::move(covers));
            measure("library_covers", chapters, sample, [&] { finish(cover_job); });
            assert(cover_job.result.covers.size() == 4);
            for (const auto& cover : cover_job.result.covers) assert(cover && !cover->empty());
            LargeEpubProbe::reclaim(path, chapters, sample, false);
            reclaims[scale] = LargeEpubProbe::reclaim(path, chapters, sample, true);
        }
        unsigned checkpoints = 0; EpubDocument cancelled; std::string error;
        assert(!cancelled.open(path.string(), error, [&] { return ++checkpoints == 8; }));
        assert(error == "EPUB open cancelled" && checkpoints == 8);
    }
    fs::remove_all(root);
    if (!baseline) {
        assert(opens[1].counts[0][work::ZipNames] <= 20 * opens[0].counts[0][work::ZipNames]);
        assert(opens[1].counts[0][work::ZipComparisons] <= 20 * opens[0].counts[0][work::ZipComparisons]);
        assert(reclaims[0].counts[0][work::ReclaimVisits] == 300);
        assert(reclaims[1].counts[0][work::ReclaimVisits] == 3000);
        assert(reclaims[0].counts[0][work::PageIndexVisits] <= 4 * 300);
        assert(reclaims[1].counts[0][work::PageIndexVisits] <= 4 * 3000);
        assert(reclaims[1].counts[0][work::PageIndexVisits] <= 12 * reclaims[0].counts[0][work::PageIndexVisits]);
    }
}
