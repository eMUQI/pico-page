#include "pico_page/sync.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace pico_page;

int main(int argc, char** argv) {
    assert(argc == 3);
    EpubDocument document;
    Chapter chapter;
    std::string error, pointer;
    assert(document.open(argv[1], error));
    assert(document.load_chapter(0, chapter, error));
    assert(chapter.plain_dom);
    for (const auto name : {"short", "boundary", "long", "spaces", "entities", "crlf", "parts", "utf16"}) {
        EpubDocument book;
        Chapter source, reading;
        const auto path = std::filesystem::path(argv[1]).parent_path() / (std::string("mapping-node-") + name + ".epub");
        assert(book.open(path.string(), error));
        for (std::uint32_t part = 0;; ++part) {
            assert(book.load_chapter(0, source, error, ChapterLoad::Mapping, part));
            assert(book.load_chapter(0, reading, error, ChapterLoad::Reading, part));
            assert(source.text == reading.text && source.last_part == reading.last_part);
            const bool refused = std::string(name) != "short";
            assert(source.long_source_node == refused);
            Anchor anchor, recovered;
            assert(source.anchor_at(0, anchor));
            for (const int version : {20240114, 20260812}) {
                assert(sync::to_xpointer(book, source, anchor, version, pointer, error) == !refused);
                if (refused) assert(pointer.empty() && error == sync::kLongSourceNode);
                const std::string prefix = "/body[1]/DocFragment[1]/body[1]/p[1]/text()[";
                assert(sync::from_xpointer(book, prefix + "1].0", version, recovered, error) == !refused);
                if (refused) {
                    assert(recovered == Anchor{} && error == sync::kLongSourceNode);
                    // A later engine text node must not resolve to the source sibling after em.
                    assert(!sync::from_xpointer(book, prefix + "2].0", version, recovered, error));
                    assert(recovered == Anchor{});
                }
            }
            if (source.last_part) break;
        }
    }
    const Anchor emoji{chapter.resource, "/html[1]/body[1]/p[1]/em[1]/text()[1]", 3};
    const Anchor leading{chapter.resource, "/html[1]/body[1]/p[1]/text()[1]", 2};
    // These expected paths and normalized text offsets were exported by the pinned CREngine.
    for (int version : {20240114, 20260812}) {
        double percentage = -1;
        assert(sync::map_upload_position(document, emoji, version, pointer, percentage, error));
        assert(pointer == "/body[1]/DocFragment[1]/body[1]/p[1]/em[1]/text()[1].3");
        assert(percentage >= 0 && percentage <= 1 && error.empty());
        Anchor recovered;
        assert(sync::from_xpointer(document, pointer, version, recovered, error));
        assert(recovered == emoji);
        assert(sync::from_xpointer(document, "/body/DocFragment[1]/body/p[1]/em/text().3", version, recovered, error));
        assert(recovered == emoji);
        assert(sync::to_xpointer(document, chapter, leading, version, pointer, error));
        assert(pointer == "/body[1]/DocFragment[1]/body[1]/p[1]/text()[1].1");
        // The reported place is the part that holds the anchor and the anchor's offset in its text.
        sync::PartPosition place;
        std::uint32_t visible = 0;
        assert(sync::from_xpointer(document, pointer, version, recovered, error, {}, &place));
        assert(recovered == leading && chapter.offset_for(leading, visible));
        assert(place.part == chapter.part && place.offset == visible && place.last_part == chapter.last_part);
        assert(place.part_scalars == chapter.text.size());
        assert(sync::from_xpointer(document, "/body/DocFragment[2]/body/p[2]/text().114", version, recovered, error));
        assert(recovered.resource == document.spine()[1].resource && recovered.offset == 114);
        assert(!sync::from_xpointer(document, "/body/DocFragment[99]/body/p/text().0", version, recovered, error));
    }
    {
        // Rejected uploads must not expose outputs retained from a previous successful mapping.
        double percentage = 1;
        pointer = "stale";
        assert(!sync::map_upload_position(document, {"missing.xhtml", emoji.node, emoji.offset},
                                          20260812, pointer, percentage, error));
        assert(pointer.empty() && percentage == 0 && error == "Saved position cannot be mapped");
        pointer = "stale"; percentage = 1;
        assert(!sync::map_upload_position(document, emoji, 20260812, pointer, percentage, error,
                                          [] { return true; }));
        assert(pointer.empty() && percentage == 0 && error == kChapterCancelled);
    }
    chapter.plain_dom = false;
    assert(!sync::to_xpointer(document, chapter, emoji, 20260812, pointer, error));
    assert(error == sync::kUnsupportedStructure);
    {
        // Element outside the profile, here a table: every position of the chapter is refused
        // in both directions with the error that names this cause.
        EpubDocument tables;
        Chapter refused_chapter;
        const auto fixtures = std::filesystem::path(argv[1]).parent_path();
        assert(tables.open((fixtures / "table-chapter.epub").string(), error) && tables.load_chapter(0, refused_chapter, error));
        assert(!refused_chapter.plain_dom && !refused_chapter.runs.empty());
        for (const auto& run : refused_chapter.runs) {
            Anchor anchor;
            assert(refused_chapter.anchor_at(run.begin, anchor));
            assert(!sync::to_xpointer(tables, refused_chapter, anchor, 20260812, pointer, error));
            assert(error == sync::kUnsupportedStructure);
        }
        Anchor refused;
        assert(!sync::from_xpointer(tables, "/body[1]/DocFragment[1]/body[1]/p[1]/text()[1].0", 20260812, refused, error));
        assert(error == sync::kUnsupportedStructure);
        // A position inside the table itself names the same cause, not a missing node.
        assert(!sync::from_xpointer(tables, "/body[1]/DocFragment[1]/body[1]/table[1]/tr[1]/td[1]/text()[1].0", 20260812, refused, error));
        assert(error == sync::kUnsupportedStructure);

        // An image stays inside the profile. Text beside it maps as before and the image maps as
        // its element at offset zero. The pinned CREngine resolves these paths to the same
        // nodes; tools/check_koreader_mapping.py repeats that comparison.
        EpubDocument images;
        Chapter flow;
        assert(images.open((fixtures / "image-flow.epub").string(), error) && images.load_chapter(0, flow, error));
        assert(flow.plain_dom && flow.images.size() == 2);
        const Anchor before{flow.resource, "/html[1]/body[1]/p[1]/text()[1]", 0};
        for (int version : {20240114, 20260812}) {
            Anchor recovered;
            sync::PartPosition place;
            std::uint32_t visible = 0;
            assert(sync::to_xpointer(images, flow, before, version, pointer, error));
            assert(pointer == "/body[1]/DocFragment[1]/body[1]/p[1]/text()[1].0");
            for (std::size_t i = 0; i < flow.images.size(); ++i) {
                const auto expected = "/body[1]/DocFragment[1]/body[1]/img[" + std::to_string(i + 1) + "].0";
                assert(sync::to_xpointer(images, flow, flow.images[i].source, version, pointer, error) && pointer == expected);
                assert(sync::from_xpointer(images, pointer, version, recovered, error, {}, &place));
                assert(recovered == flow.images[i].source && flow.offset_for(recovered, visible));
                assert(place.part == flow.part && place.offset == visible && visible == flow.images[i].begin);
            }
            // The engine omits a first index in the earlier DOM version.
            assert(sync::from_xpointer(images, "/body/DocFragment[1]/body/img.0", version, recovered, error));
            assert(recovered == flow.images[0].source);
            // An image position has no offset other than zero, and the image must exist.
            assert(!sync::from_xpointer(images, "/body[1]/DocFragment[1]/body[1]/img[1].1", version, recovered, error));
            assert(!sync::from_xpointer(images, "/body[1]/DocFragment[1]/body[1]/img[3].0", version, recovered, error));
            assert(!sync::from_xpointer(images, "/body[1]/DocFragment[1]/body[1]/p[1]/img[1].0", version, recovered, error));
            assert(recovered == Anchor{});
        }

        // A stylesheet that can remove elements from the layout leaves the visibility of an image
        // unknown, so its position is refused in both directions; text keeps its mapping. A
        // display value other than none does not hide anything.
        EpubDocument hiding;
        Chapter hidden, shown;
        assert(hiding.open((fixtures / "hidden-image.epub").string(), error));
        assert(hiding.load_chapter(0, hidden, error) && hiding.load_chapter(1, shown, error));
        assert(hidden.images.size() == 1 && shown.images.size() == 1);
        Anchor unused;
        assert(!sync::to_xpointer(hiding, hidden, hidden.images[0].source, 20260812, pointer, error));
        assert(error == std::string(sync::kUnresolvedVisibility));
        assert(!sync::from_xpointer(hiding, "/body[1]/DocFragment[1]/body[1]/img[1].0", 20260812, unused, error));
        assert(error == std::string(sync::kUnresolvedVisibility));
        assert(sync::to_xpointer(hiding, hidden, {hidden.resource, "/html[1]/body[1]/p[1]/text()[1]", 0}, 20260812, pointer, error));
        assert(sync::to_xpointer(hiding, shown, shown.images[0].source, 20260812, pointer, error));
        assert(pointer == "/body[1]/DocFragment[2]/body[1]/img[1].0");
        assert(sync::from_xpointer(hiding, pointer, 20260812, unused, error) && unused == shown.images[0].source);

        // An image that opens a chapter part lies inside that part: its run, its image entry and
        // the place reported for its position all use offsets of the part that holds it.
        EpubDocument split;
        Chapter second;
        assert(split.open((fixtures / "part-boundary-image.epub").string(), error));
        assert(split.load_chapter(0, second, error, ChapterLoad::Mapping, 1));
        assert(second.part == 1 && second.last_part && second.images.size() == 1 && second.plain_dom);
        const auto& opening = second.images[0];
        assert(opening.begin == 0 && opening.begin < opening.end && opening.end <= second.text.size());
        assert(!second.runs.empty() && second.runs[0].element_reference && second.runs[0].begin == 0 &&
               second.runs[0].end == opening.end);
        Anchor first_anchor;
        assert(second.anchor_at(0, first_anchor) && first_anchor == opening.source);
        Anchor recovered;
        sync::PartPosition place;
        assert(sync::from_xpointer(split, "/body[1]/DocFragment[1]/body[1]/img[1].0", 20260812, recovered, error, {}, &place));
        assert(recovered == opening.source && place.part == 1 && place.offset == 0 && place.offset < place.part_scalars);
    }
    chapter.plain_dom = true;
    chapter.css_sources.push_back("p { white-space: pre-wrap; }");
    assert(!sync::to_xpointer(document, chapter, emoji, 20260812, pointer, error));
    chapter.css_sources.clear();
    EpubDocument styled;
    const auto mapping_path = std::filesystem::path(argv[1]).parent_path() / "mapping.epub";
    assert(styled.open(mapping_path.string(), error));
    Chapter mapped;
    assert(styled.load_chapter(0, mapped, error));
    assert(mapped.plain_dom && mapped.css_sources.size() == 3);
    const Anchor after_inline{mapped.resource, "/html[1]/body[1]/p[2]/text()[2]", 1};
    assert(sync::to_xpointer(styled, mapped, after_inline, 20260812, pointer, error));
    assert(pointer == "/body[1]/DocFragment[1]/body[1]/p[2]/text()[1].1");
    Anchor back;
    assert(sync::from_xpointer(styled, pointer, 20260812, back, error) && back == after_inline);

    sync::Config config{"https://sync.example.invalid", "test-user", "test-key", "test-device", 20260812};
    sync::HttpRequest request;
    assert(sync::get_request(config, document.metadata().partial_md5, request, error));
    assert(request.method == "GET" && request.body.empty());
    ReadingRecord saved{document.metadata().book_id, "epub-native-v1", "test-writer", 7, emoji};
    assert(sync::put_request(config, document.metadata().partial_md5,
           "/body[1]/DocFragment[1]/body[1]/p[1]/em[1]/text()[1].3", 0.2, saved, request, error));
    saved.local_revision = 8;
    assert(request.uploaded_record && request.uploaded_record->local_revision == 7);
    assert(request.body.find("test-key") == std::string::npos);
    assert(sync::upload_confirmed(200, error));
    assert(!sync::upload_confirmed(202, error));
    assert(sync::parse_remote(200, "{}").status == sync::RemoteStatus::NoRecord);
    assert(sync::parse_remote(401, "private server error").status == sync::RemoteStatus::Unauthorized);
    assert(sync::parse_remote(200, R"({"progress":"/body/a.1","percentage":0.2})").status == sync::RemoteStatus::Found);
    assert(sync::parse_remote(200, R"({"progress":"x","percentage":0.1,"percentage":0.2})").status == sync::RemoteStatus::Error);
    assert(sync::parse_remote(200, R"({"progress":"x","percentage":2})").status == sync::RemoteStatus::Error);
    assert(sync::parse_remote(200, R"({"error":"server error"})").status == sync::RemoteStatus::Error);
    config.endpoint = "http://sync.example.invalid";
    assert(!sync::get_request(config, document.metadata().partial_md5, request, error));
    char directory[] = "/tmp/pico-page-sync-XXXXXX";
    assert(mkdtemp(directory));
    const auto config_dir = std::filesystem::path(directory) / "PicoPage";
    std::filesystem::create_directory(config_dir);
    {
        std::ofstream file(config_dir / "sync.toml");
        file << "schema_version = 1\ndom_version = 20260812\nendpoint = \"https://sync.example.invalid/\"\n"
                "username = \"test\"\nuserkey = \"test-private-key\"\ndevice_id = \"pico-test\"\n";
    }
    assert(sync::load_config(directory, config, error));
    assert(config.endpoint == "https://sync.example.invalid");
    { std::ofstream file(config_dir / "sync.toml"); file << "userkey = \"test-private-key"; }
    assert(!sync::load_config(directory, config, error));
    assert(config.userkey.empty() && error.find("test-private-key") == std::string::npos);
    std::filesystem::remove_all(directory);
    // argv[2] is a card written by tools/setup_network.py; see tests/test_setup_network.py.
    assert(sync::load_config(argv[2], config, error));
    assert(config.endpoint == "https://sync.example.invalid" && config.username == "读者 \"a\\b\"" &&
           config.userkey == "55d2fddf8ba5768bbc44b083846cf439" && config.device_id == "DEVICE-1" &&
           config.dom_version == 20240114);
    std::cout << "CREngine-derived text mapping and bounded KOSync protocol checks passed\n";
}
