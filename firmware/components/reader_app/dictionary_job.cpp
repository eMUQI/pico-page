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
    std::string path;
    const auto resolved = store.dictionary_path(request.relative_path, path);
    if (resolved != StoreStatus::Ok) { finish(path_error(resolved)); return; }
    if (request.kind == Kind::DictionaryLookup && (request.dictionary_candidates.empty() || request.dictionary_candidates.size() > 36)) {
        finish({dictionary::Status::InvalidFormat, "Invalid dictionary candidates"}); return;
    }
    auto* query = request.kind == Kind::DictionaryLookup ? request.dictionary_query.get() : nullptr;
    if (query) {
        const auto& failures = query->preparation_failures;
        const auto found = std::find_if(failures.begin(), failures.end(),
            [&](const auto& entry) { return entry.first == request.relative_path; });
        if (found != failures.end()) { finish(found->second); return; }
    }
    dictionary::Verification temporary;
    auto* verification = &temporary;
    if (request.dictionary_session) {
        auto& entries = request.dictionary_session->verified;
        auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.first == request.relative_path; });
        if (found == entries.end()) {
            if (entries.size() == 32) { finish({dictionary::Status::ResourceLimit, "Too many verified dictionaries"}); return; }
            entries.emplace_back(request.relative_path, dictionary::Verification{});
            found = std::prev(entries.end());
        }
        verification = &found->second;
    }
    dictionary::Dictionary reader;
    auto status = reader.open_prepared(path, store, *verification, stop);
    if (!status) {
        if (query && query->preparation_failures.size() < 32 &&
            (status.status == dictionary::Status::InvalidFormat || status.status == dictionary::Status::Unsupported))
            query->preparation_failures.emplace_back(request.relative_path, status);
        finish(std::move(status)); return;
    }
    result.dictionary_info = reader.info(); result.dictionary_reused = reader.reused_index();
    if (request.kind == Kind::DictionaryPrepare) return;
    if (request.kind == Kind::DictionaryRead) {
        finish(reader.read(request.dictionary_ordinal, result.dictionary_fields, stop)); return;
    }
    for (const auto& candidate : request.dictionary_candidates) {
        if (candidate.word.size() > 255 || candidate.selection.begin >= candidate.selection.end ||
            candidate.selection.end - candidate.selection.begin > 64) {
            finish({dictionary::Status::InvalidFormat, "Invalid dictionary candidate"}); return;
        }
        status = reader.lookup(candidate.word, result.dictionary_entries, result.dictionary_more,
                               request.dictionary_ordinal, stop);
        if (status.status == dictionary::Status::NotFound) continue;
        if (!status) { finish(std::move(status)); return; }
        result.dictionary_match = candidate;
        finish(reader.read(result.dictionary_entries.front().ordinal, result.dictionary_fields, stop));
        return;
    }
    finish({dictionary::Status::NotFound, "No dictionary entry found"});
}

}  // namespace pico_page
