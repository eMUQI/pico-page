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
    // results therefore skip path checks. Preparation always hashes the source; explicit retry
    // hashes only a source whose definitive failure it clears.
    auto* session = request.dictionary_session.get();
    bool full_check = request.kind == Kind::DictionaryPrepare;
    if (session) {
        auto& failures = session->preparation_failures;
        const auto found = std::find_if(failures.begin(), failures.end(),
            [&](const auto& entry) { return entry.first == relative_path; });
        if (found != failures.end()) {
            if (!request.dictionary_retry && request.kind != Kind::DictionaryPrepare) return found->second;
            failures.erase(found); full_check = true;
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
    auto status = reader.open_prepared(source->path, store, source->verification, stop, full_check);
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
    if (sources.size() > 32) {
        result.dictionary_status = {dictionary::Status::ResourceLimit, "Too many dictionary sources"};
        result.error = result.dictionary_status.message; return;
    }
    // Candidates arrive best first. A source may only replace the current match with a
    // strictly better candidate, so equal candidates keep the earlier source.
    std::size_t limit = candidates.size();
    dictionary::Result failure;
    bool found = false, absent_candidate = false;
    for (std::size_t source = 0; source < sources.size() && limit; ++source) {
        if (stop()) break;
        dictionary::Dictionary reader;
        auto status = open_dictionary(store, sources[source], reader, stop);
        if (status.status == dictionary::Status::InvalidFormat || status.status == dictionary::Status::Unsupported)
            result.dictionary_failed_sources.push_back({sources[source], {}, status});
        std::vector<dictionary::Entry> entries, matched;
        std::vector<dictionary::Field> fields;
        bool more = false, matched_more = false;
        std::size_t hit = limit;
        // Exact candidates sharing a start are prefixes of each other. Querying them from
        // short to long lets a prefix that no headword extends rule out every longer one.
        std::vector<std::size_t> order(limit);
        for (std::size_t index = 0; index < limit; ++index) order[index] = index;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const auto& x = candidates[a]; const auto& y = candidates[b];
            if (x.inflection != y.inflection) return !x.inflection;
            if (x.selection.begin != y.selection.begin) return x.selection.begin < y.selection.begin;
            return x.selection.end < y.selection.end;
        });
        std::vector<const dictionary::Candidate*> closed;  // Candidates no headword extends.
        std::vector<bool> settled(limit);  // Candidates known absent or unusable in this source.
        const auto fail = [&](dictionary::Result error) {
            if (failure) failure = std::move(error);  // Keep the first failure.
            result.dictionary_partial = true;
        };
        // A failed lookup or entry read leaves lower-ranked candidates in this source available.
        while (status) {
            hit = limit;
            for (const auto index : order) {
                const auto& candidate = candidates[index];
                if (index > hit || settled[index]) continue;  // Cannot outrank the current match.
                if (!candidate.inflection && std::any_of(closed.begin(), closed.end(), [&](const auto* prefix) {
                        return prefix->selection.begin == candidate.selection.begin &&
                               candidate.word.size() > prefix->word.size() &&
                               candidate.word.compare(0, prefix->word.size(), prefix->word) == 0;
                    })) continue;
                bool extensible = true;
                auto lookup = reader.lookup(candidate.word, entries, more, request.dictionary_ordinal, stop, &extensible);
                if (lookup.status == dictionary::Status::NotFound) {
                    absent_candidate = true;
                    settled[index] = true;
                    if (!candidate.inflection && !extensible) closed.push_back(&candidate);
                    continue;
                }
                if (lookup.status == dictionary::Status::Cancelled) { status = std::move(lookup); break; }
                if (!lookup) { settled[index] = true; fail(std::move(lookup)); continue; }
                hit = index; matched = std::move(entries); matched_more = more;
            }
            if (!status || hit == limit) break;
            auto read = reader.read(matched.front().ordinal, fields, stop);
            if (read) { entries = std::move(matched); more = matched_more; status = std::move(read); break; }
            if (read.status == dictionary::Status::Cancelled) { status = std::move(read); break; }
            settled[hit] = true; fail(std::move(read));
        }
        if (status.status == dictionary::Status::Cancelled || stop()) break;
        if (!status) { fail(std::move(status)); continue; }
        if (hit == limit) continue;
        found = true;
        result.dictionary_source = source;
        result.dictionary_info = reader.info(); result.dictionary_reused = reader.reused_index();
        result.dictionary_match = candidates[hit];
        result.dictionary_entries = std::move(entries); result.dictionary_more = more;
        result.dictionary_fields = std::move(fields);
        result.dictionary_status = std::move(status);  // Retains omitted_fields from the read.
        if (request.dictionary_navigation) {
            if (stop()) break;
            return;
        }
        limit = hit;
        while (limit && same_rank(candidates[limit - 1], candidates[hit])) --limit;
    }
    if (stop()) {
        result.dictionary_match.reset(); result.dictionary_entries.clear(); result.dictionary_fields.clear();
        result.dictionary_status = {dictionary::Status::Cancelled, "Dictionary operation cancelled"};
        result.error = result.dictionary_status.message; return;
    }
    if (found) return;
    // Report a read failure when no source could establish a genuine absence.
    if (!failure && (sources.size() == 1 || !absent_candidate)) {
        result.dictionary_status = std::move(failure); result.error = result.dictionary_status.message;
    }
    else result.dictionary_status = {dictionary::Status::NotFound, "No dictionary entry found"};
}

}  // namespace pico_page
