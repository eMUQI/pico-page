#include "content_job.h"
#include "pico_page/stage_timer.h"
#include <algorithm>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace pico_page {
namespace {
dictionary::Result path_error(StoreStatus status) {
    return {status == StoreStatus::UnsafePath ? dictionary::Status::InvalidFormat : dictionary::Status::IoError,
            status == StoreStatus::Missing ? "Dictionary source is missing" :
            status == StoreStatus::UnsafePath ? "Dictionary source path is unsafe" : "Dictionary source cannot be read"};
}
}  // namespace

void ContentJob::read_dictionary() {
    const StageTimer timer{"dictionary_job"};
    FileStore store(request.root);
    const auto stop = [this] { return checkpoint(); };
    const auto finish = [&](dictionary::Result status) {
        result.dictionary_status = std::move(status);
        if (!result.dictionary_status && result.dictionary_status.status != dictionary::Status::NotFound)
            result.error = result.dictionary_status.message;
    };
    if (stop()) { finish({dictionary::Status::Cancelled, "Dictionary operation cancelled"}); return; }
    if (request.kind == Kind::DictionaryPage) {
        if (!request.dictionary_definition) { finish({dictionary::Status::InvalidFormat, "No dictionary definition"}); return; }
#ifdef ESP_PLATFORM
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < (768U + 512U) * 1024 ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 256U * 1024) {
            finish({dictionary::Status::ResourceLimit, "Insufficient memory for dictionary layout"}); return;
        }
#endif
        graphics::Font font;
        if ((!request.font.empty() && !font.open(request.font)) || (request.font.empty() && !font.open_fallback())) {
            finish({dictionary::Status::IoError, "Dictionary font cannot be opened"}); return;
        }
        auto cursor = request.dictionary_cursor;
        if (request.dictionary_seek_pages > 131072) { finish({dictionary::Status::ResourceLimit, "Dictionary page seek exceeds its limit"}); return; }
        const auto last_page = request.dictionary_reflow ? 131072U : request.dictionary_seek_pages;
        for (std::uint32_t index = 0; index <= last_page; ++index) {
            const auto start = cursor;
            dictionary::TextChunk chunk;
            const auto limit = cursor.scalars < request.dictionary_layout_anchor ?
                std::min<std::size_t>(4096, request.dictionary_layout_anchor - cursor.scalars) : 4096;
            auto status = dictionary::text_chunk(*request.dictionary_definition, cursor, limit, chunk, stop);
            if (!status) { finish(std::move(status)); return; }
            if (chunk.text.empty()) {
                if ((request.dictionary_reflow && cursor.scalars < request.dictionary_layout_anchor) ||
                    index < request.dictionary_seek_pages) finish({dictionary::Status::NotFound, "Dictionary page does not exist"});
                else { result.dictionary_complete = true; result.dictionary_current = start; result.dictionary_next = cursor; }
                return;
            }
            auto page = graphics::layout_page(font, chunk.text, 0, request.layout, true, stop);
            if (!page.error.empty()) {
                finish({stop() ? dictionary::Status::Cancelled : dictionary::Status::ResourceLimit, page.error}); return;
            }
            for (auto& glyph : page.glyphs) glyph.bold = chunk.emphasis[glyph.index];
            const auto count = page.end;
            if (!count) { finish({dictionary::Status::ResourceLimit, "Dictionary layout made no progress"}); return; }
            if (count < chunk.text.size()) {
                cursor = start;
                status = dictionary::text_chunk(*request.dictionary_definition, cursor, count, chunk, stop);
                if (!status) { finish(std::move(status)); return; }
            }
            result.dictionary_next = cursor;
            auto lookahead = cursor;
            status = dictionary::text_chunk(*request.dictionary_definition, lookahead, 1, chunk, stop);
            if (!status) { finish(std::move(status)); return; }
            if (request.dictionary_reflow ? start.scalars < request.dictionary_layout_anchor : index < request.dictionary_seek_pages) {
                if (chunk.end) { finish({dictionary::Status::NotFound, "Dictionary page does not exist"}); return; }
                continue;
            }
            result.dictionary_page_number = index;
            result.dictionary_current = start;
            result.dictionary_complete = chunk.end;
            result.dictionary_missing_glyphs = std::any_of(page.glyphs.begin(), page.glyphs.end(),
                [&](const auto& glyph) { return font.missing(glyph.scalar); });
            result.dictionary_page = std::move(page);
            return;
        }
        finish({dictionary::Status::ResourceLimit, "Dictionary reflow exceeds its limit"});
        return;
    }
    if (request.kind == Kind::DictionaryList) {
        std::vector<std::string> names;
        const auto status = store.dictionary_files(names, result.dictionary_complete, stop);
        if (stop()) { finish({dictionary::Status::Cancelled, "Dictionary scan cancelled"}); return; }
        if (status == StoreStatus::Missing) { result.dictionary_complete = true; return; }
        if (status != StoreStatus::Ok) { finish(path_error(status)); return; }
        for (auto& id : names) {
            if (stop()) {
                result.dictionaries.clear(); result.dictionary_complete = false;
                finish({dictionary::Status::Cancelled, "Dictionary scan cancelled"}); return;
            }
            DictionaryItem item;
            item.id = std::move(id);
            std::string path;
            const auto resolved = store.dictionary_path(item.id, path);
            item.status = resolved == StoreStatus::Ok ? dictionary::inspect(path, item.info, stop) : path_error(resolved);
            if (item.status.status == dictionary::Status::Cancelled) {
                result.dictionaries.clear(); result.dictionary_complete = false; finish(std::move(item.status)); return;
            }
            result.dictionaries.push_back(std::move(item));
        }
        return;
    }
    if (request.kind == Kind::DictionaryLookup) { search_dictionaries(store, stop); return; }
    dictionary::Dictionary reader;
    auto status = open_dictionary(store, request.relative_path, reader, stop);
    if (!status) { finish(std::move(status)); return; }
    result.dictionary_info = reader.info(); result.dictionary_reused = reader.reused_index();
    if (request.kind == Kind::DictionaryPrepare) return;
    finish(reader.read(request.dictionary_ordinal, result.dictionary_fields, stop));
}

dictionary::Result ContentJob::open_dictionary(FileStore& store, const std::string& relative_path,
                                               dictionary::Dictionary& reader, const dictionary::Stop& stop) {
    // Sources cannot change within a mount session: USB and remount reset it. Remembered
    // results therefore skip path checks; explicit retry and preparation re-read the source.
    auto* session = request.dictionary_session.get();
    if (session) {
        auto& failures = session->preparation_failures;
        const auto found = std::find_if(failures.begin(), failures.end(),
            [&](const auto& entry) { return entry.first == relative_path; });
        if (found != failures.end()) {
            if (!request.dictionary_retry && request.kind != Kind::DictionaryPrepare) return found->second;
            failures.erase(found);
        }
    }
    DictionarySession::Source temporary;
    auto* source = &temporary;
    if (session) {
        auto& sources = session->verified;
        auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& entry) { return entry.relative_path == relative_path; });
        if (found == sources.end()) {
            if (sources.size() == 32) return {dictionary::Status::ResourceLimit, "Too many verified dictionaries"};
            sources.push_back({relative_path, {}, {}});
            found = std::prev(sources.end());
        }
        source = &*found;
    }
    if (source->path.empty() || request.kind == Kind::DictionaryPrepare) {
        source->path.clear();
        std::string path;
        const auto resolved = store.dictionary_path(relative_path, path);
        if (resolved != StoreStatus::Ok) return path_error(resolved);
        source->path = std::move(path);
    }
    auto status = reader.open_prepared(source->path, store, source->verification, stop);
    if (!status) {
        source->path.clear();
        if (session && session->preparation_failures.size() < 32 &&
            (status.status == dictionary::Status::InvalidFormat || status.status == dictionary::Status::Unsupported))
            session->preparation_failures.emplace_back(relative_path, status);
    }
    return status;
}

void ContentJob::search_dictionaries(FileStore& store, const dictionary::Stop& stop) {
    const auto& candidates = request.dictionary_candidates;
    if (candidates.empty() || candidates.size() > 36) {
        result.dictionary_status = {dictionary::Status::InvalidFormat, "Invalid dictionary candidates"};
        result.error = result.dictionary_status.message; return;
    }
    for (const auto& candidate : candidates)
        if (candidate.word.size() > 255 || candidate.selection.begin >= candidate.selection.end ||
            candidate.selection.end - candidate.selection.begin > 64) {
            result.dictionary_status = {dictionary::Status::InvalidFormat, "Invalid dictionary candidate"};
            result.error = result.dictionary_status.message; return;
        }
    const auto same_rank = [](const dictionary::Candidate& a, const dictionary::Candidate& b) {
        return a.inflection == b.inflection &&
               a.selection.end - a.selection.begin == b.selection.end - b.selection.begin;
    };
    const std::vector<std::string> single{request.relative_path};
    const auto& sources = request.dictionary_sources.empty() ? single : request.dictionary_sources;
    // Candidates arrive best first. A source may only replace the current match with a
    // strictly better candidate, so equal candidates keep the earlier source.
    std::size_t limit = candidates.size();
    dictionary::Result failure;
    bool found = false;
    for (std::size_t source = 0; source < sources.size() && limit; ++source) {
        if (stop()) break;
        dictionary::Dictionary reader;
        auto status = open_dictionary(store, sources[source], reader, stop);
        std::vector<dictionary::Entry> entries;
        std::vector<dictionary::Field> fields;
        bool more = false;
        std::size_t hit = limit;
        for (std::size_t index = 0; status && index < limit; ++index) {
            auto lookup = reader.lookup(candidates[index].word, entries, more, request.dictionary_ordinal, stop);
            if (lookup.status == dictionary::Status::NotFound) continue;
            if (lookup) lookup = reader.read(entries.front().ordinal, fields, stop);
            if (lookup) { hit = index; status = std::move(lookup); break; }
            if (lookup.status == dictionary::Status::Cancelled) { status = std::move(lookup); break; }
            // An unreadable entry leaves lower-ranked candidates in this source available.
            if (failure) failure = std::move(lookup);
            result.dictionary_partial = true;
        }
        if (status.status == dictionary::Status::Cancelled || stop()) break;
        if (!status) {
            if (failure) failure = std::move(status);  // Keep the first failure.
            result.dictionary_partial = true;
            continue;
        }
        if (hit == limit) continue;
        found = true;
        result.dictionary_source = source;
        result.dictionary_info = reader.info(); result.dictionary_reused = reader.reused_index();
        result.dictionary_match = candidates[hit];
        result.dictionary_entries = std::move(entries); result.dictionary_more = more;
        result.dictionary_fields = std::move(fields);
        result.dictionary_status = std::move(status);  // Retains omitted_fields from the read.
        limit = hit;
        while (limit && same_rank(candidates[limit - 1], candidates[hit])) --limit;
    }
    if (stop()) {
        result.dictionary_match.reset(); result.dictionary_entries.clear(); result.dictionary_fields.clear();
        result.dictionary_status = {dictionary::Status::Cancelled, "Dictionary operation cancelled"};
        result.error = result.dictionary_status.message; return;
    }
    if (found) return;
    // One explicitly chosen source reports its own failure; a search of several reports a partial miss.
    if (sources.size() == 1 && !failure) { result.dictionary_status = std::move(failure); result.error = result.dictionary_status.message; }
    else result.dictionary_status = {dictionary::Status::NotFound, "No dictionary entry found"};
}

}  // namespace pico_page
