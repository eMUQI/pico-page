#pragma once
#include "pico_page/document.h"
#include "pico_page/store.h"
#include <optional>
namespace pico_page {
inline constexpr std::size_t kPageIndexBytes = 1U << 20;
using PageIndices = std::vector<std::vector<std::vector<std::uint32_t>>>;
struct PageCache { PageIndices pages; std::vector<bool> counted; };
/** Matching identity header, integrity and expanded allocation bounds are required before adoption. */
std::optional<PageCache> read_page_cache(const FileStore& store, const std::string& name, const std::string& header,
    const std::vector<SpineItem>& spine, const std::function<bool()>& stop = {});
bool write_page_cache(const FileStore& store, const std::string& name, const std::string& header,
    const PageIndices& pages, const std::vector<bool>& counted, const std::function<bool()>& stop = {});
}
