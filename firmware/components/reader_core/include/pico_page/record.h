#pragma once

#include <cstdint>
#include <string>

namespace pico_page {

/** Source position independent of layout. Offset is measured in Unicode scalars. */
struct Anchor {
    std::string resource;
    std::string node;
    std::uint32_t offset = 0;
};

inline bool operator==(const Anchor& a, const Anchor& b) {
    return a.resource == b.resource && a.node == b.node && a.offset == b.offset;
}
inline bool operator!=(const Anchor& a, const Anchor& b) { return !(a == b); }

/** A successfully presented position. Persistence acknowledgement is separate. */
struct ReadingRecord {
    std::string book_id;
    std::string content_revision;
    std::string writer_id;
    std::uint64_t local_revision = 0;
    Anchor anchor;
};

inline bool operator==(const ReadingRecord& a, const ReadingRecord& b) {
    return a.book_id == b.book_id && a.content_revision == b.content_revision &&
           a.writer_id == b.writer_id && a.local_revision == b.local_revision &&
           a.anchor == b.anchor;
}

}  // namespace pico_page
