#pragma once
#include "pico_page/document.h"
#include "pico_page/store.h"

namespace pico_page {
/** One rebuildable chapter payload, owned by the caller until its optional idle write. */
struct ContentCacheEntry {
    std::string name, last_name, bytes;
};
bool save_content_cache(const FileStore& store, const ContentCacheEntry& entry,
                        const std::function<bool()>& stop = {});
// Rebuildable Reading blocks retain source anchors; Mapping reads the original book.
// With deferred, the requested part is encoded for an idle write; skipped prefix parts
// retain their synchronous cache writes so later anchor searches can use the target.
bool load_content(const FileStore& store, EpubDocument& document, std::size_t index,
                  Chapter& chapter, std::string& error, ChapterLoad detail, std::uint32_t part,
                  const std::function<bool()>& stop = {}, ContentCacheEntry* deferred = nullptr);
bool find_content(const FileStore& store, EpubDocument& document, std::size_t index,
                  Chapter& chapter, std::string& error, ChapterLoad detail,
                  const std::function<bool(const Chapter&)>& match, const std::function<bool()>& stop = {},
                  ContentCacheEntry* deferred = nullptr);
} // namespace pico_page
