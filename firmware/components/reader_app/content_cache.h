#pragma once
#include "pico_page/document.h"
#include "pico_page/store.h"

namespace pico_page {
/** One rebuildable chapter payload, owned by the caller until its optional idle write. */
struct ContentCacheEntry {
    std::string name, last_name, bytes;
};
/** Tests the versioned identity/index/part key without reading or trusting the payload. */
bool content_cache_matches(const ContentCacheEntry& entry, const std::string& book_id,
                           std::size_t index, std::uint32_t part);
/** Reads a Reading part using a resource from the already verified open book.
 * Validates the digest, identity, resource and records before publishing chapter. A retained
 * entry replaces the file read. False leaves chapter empty; callers may reopen the source.
 */
bool load_content_cache(const FileStore& store, const std::string& book_id, const std::string& resource,
                        std::size_t index, std::uint32_t part, Chapter& chapter,
                        const std::function<bool()>& stop = {}, const ContentCacheEntry* retained = nullptr);
bool save_content_cache(const FileStore& store, const ContentCacheEntry& entry,
                        const std::function<bool()>& stop = {});
// Rebuildable Reading blocks retain source anchors; Mapping reads the original book.
// With deferred, the requested part is encoded for an idle write; skipped prefix parts
// are cached only when absent; anchor searches repair invalid prefix entries on demand.
// cache_checked skips a disk lookup already attempted with this verified identity/resource.
bool load_content(const FileStore& store, EpubDocument& document, std::size_t index,
                  Chapter& chapter, std::string& error, ChapterLoad detail, std::uint32_t part,
                  const std::function<bool()>& stop = {}, ContentCacheEntry* deferred = nullptr,
                  bool cache_checked = false);
bool find_content(const FileStore& store, EpubDocument& document, std::size_t index,
                  Chapter& chapter, std::string& error, ChapterLoad detail,
                  const std::function<bool(const Chapter&)>& match, const std::function<bool()>& stop = {},
                  ContentCacheEntry* deferred = nullptr);
} // namespace pico_page
