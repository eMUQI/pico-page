#pragma once
#include "pico_page/document.h"
#include "pico_page/store.h"

namespace pico_page {
struct ContentCacheEntry {
    std::string name, last_name, bytes;
    bool from_disk = false;
};
bool content_cache_matches(const ContentCacheEntry& entry, const std::string& book_id,
                           std::size_t index, std::uint32_t part);
bool load_content_cache(const FileStore& store, const std::string& book_id, const std::string& resource,
                        std::size_t index, std::uint32_t part, Chapter& chapter,
                        const std::function<bool()>& stop = {}, const ContentCacheEntry* retained = nullptr,
                        ContentCacheEntry* loaded = nullptr);
bool save_content_cache(const FileStore& store, const ContentCacheEntry& entry,
                        const std::function<bool()>& stop = {});
bool load_content(const FileStore& store, EpubDocument& document, std::size_t index,
                  Chapter& chapter, std::string& error, ChapterLoad detail, std::uint32_t part,
                  const std::function<bool()>& stop = {}, ContentCacheEntry* deferred = nullptr,
                  bool cache_checked = false, bool sequential = false);
bool find_content(const FileStore& store, EpubDocument& document, std::size_t index,
                  Chapter& chapter, std::string& error, ChapterLoad detail,
                  const std::function<bool(const Chapter&)>& match, const std::function<bool()>& stop = {},
                  ContentCacheEntry* deferred = nullptr, bool sequential = false);
}
