#pragma once
#include "pico_page/document.h"
#include "pico_page/store.h"

namespace pico_page {
// Rebuildable Reading blocks retain source anchors; Mapping always uses the original EPUB.
bool load_content(const FileStore& store, EpubDocument& document, std::size_t index,
                  Chapter& chapter, std::string& error, ChapterLoad detail, std::uint32_t part,
                  const std::function<bool()>& stop = {});
bool find_content(const FileStore& store, EpubDocument& document, std::size_t index,
                  Chapter& chapter, std::string& error, ChapterLoad detail,
                  const std::function<bool(const Chapter&)>& match);
} // namespace pico_page
