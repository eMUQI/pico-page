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
    const Anchor emoji{chapter.resource, "/html[1]/body[1]/p[1]/em[1]/text()[1]", 3};
    const Anchor leading{chapter.resource, "/html[1]/body[1]/p[1]/text()[1]", 2};
    // These expected paths and normalized text offsets were exported by the pinned CREngine.
    for (int version : {20240114, 20260812}) {
        assert(sync::to_xpointer(document, chapter, emoji, version, pointer, error));
        assert(pointer == "/body[1]/DocFragment[1]/body[1]/p[1]/em[1]/text()[1].3");
        Anchor recovered;
        assert(sync::from_xpointer(document, pointer, version, recovered, error));
        assert(recovered == emoji);
        assert(sync::from_xpointer(document, "/body/DocFragment[1]/body/p[1]/em/text().3", version, recovered, error));
        assert(recovered == emoji);
        assert(sync::to_xpointer(document, chapter, leading, version, pointer, error));
        assert(pointer == "/body[1]/DocFragment[1]/body[1]/p[1]/text()[1].1");
        assert(sync::from_xpointer(document, pointer, version, recovered, error));
        assert(recovered == leading);
        assert(sync::from_xpointer(document, "/body/DocFragment[2]/body/p[2]/text().114", version, recovered, error));
        assert(recovered.resource == document.spine()[1].resource && recovered.offset == 114);
        assert(!sync::from_xpointer(document, "/body/DocFragment[99]/body/p/text().0", version, recovered, error));
    }
    chapter.plain_dom = false;
    assert(!sync::to_xpointer(document, chapter, emoji, 20260812, pointer, error));
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
