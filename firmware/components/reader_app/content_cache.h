#pragma once
#include "pico_page/document.h"
#include "pico_page/store.h"

namespace pico_page {
/** Conservative retained capacity, including inline string capacity but excluding allocator bookkeeping. */
std::size_t chapter_cache_bytes(const Chapter& chapter);
// Rebuildable Reading blocks retain source anchors; Mapping reads the original book.
bool load_content(const FileStore& store, EpubDocument& document, std::size_t index,
                  Chapter& chapter, std::string& error, ChapterLoad detail, std::uint32_t part,
                  const std::function<bool()>& stop = {});
bool find_content(const FileStore& store, EpubDocument& document, std::size_t index,
                  Chapter& chapter, std::string& error, ChapterLoad detail,
                  const std::function<bool(const Chapter&)>& match, const std::function<bool()>& stop = {});
} // namespace pico_page
