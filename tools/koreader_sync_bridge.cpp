#include "pico_page/sync.h"
#include "cJSON.h"

#include <cstdlib>
#include <iostream>
#include <iterator>

namespace {
void require(bool condition, const std::string& error) {
    if (!condition) { std::cerr << error << '\n'; std::exit(1); }
}
std::string utf8(const std::u32string& input) {
    std::string result;
    for (char32_t c : input) {
        if (c < 0x80) result += static_cast<char>(c);
        else if (c < 0x800) { result += static_cast<char>(0xc0 | c >> 6); result += static_cast<char>(0x80 | (c & 63)); }
        else if (c < 0x10000) { result += static_cast<char>(0xe0 | c >> 12); result += static_cast<char>(0x80 | ((c >> 6) & 63)); result += static_cast<char>(0x80 | (c & 63)); }
        else { result += static_cast<char>(0xf0 | c >> 18); result += static_cast<char>(0x80 | ((c >> 12) & 63)); result += static_cast<char>(0x80 | ((c >> 6) & 63)); result += static_cast<char>(0x80 | (c & 63)); }
    }
    return result;
}
void string(cJSON* output, const char* key, const std::string& value) {
    cJSON_AddStringToObject(output, key, value.c_str());
}
}

int main(int argc, char** argv) {
    require(argc == 3, "Usage: koreader_sync_bridge book.epub offer|receive");
    pico_page::EpubDocument document;
    std::string error, pointer;
    require(document.open(argv[1], error), error);
    pico_page::Anchor anchor;
    pico_page::Chapter chapter;
    cJSON* output = cJSON_CreateObject();
    string(output, "document", document.metadata().partial_md5);
    if (std::string(argv[2]) == "offer") {
        bool found = false;
        for (std::size_t index = 0; index < document.spine().size() && !found; ++index) {
            require(document.load_chapter(index, chapter, error), error);
            for (std::uint32_t pos = chapter.text.size() * 2 / 3; pos + 24 < chapter.text.size(); ++pos) {
                pico_page::Anchor end;
                std::string end_pointer;
                if (!chapter.anchor_at(pos, anchor) || !chapter.anchor_at(pos + 24, end) ||
                    anchor.node != end.node || end.offset != anchor.offset + 24 ||
                    !pico_page::sync::to_xpointer(document, chapter, anchor, 20260812, pointer, error) ||
                    !pico_page::sync::to_xpointer(document, chapter, end, 20260812, end_pointer, error)) continue;
                string(output, "end_pointer", end_pointer);
                string(output, "text", utf8(chapter.text.substr(pos, 24)));
                found = true;
                break;
            }
        }
        require(found, "No sufficiently long supported passage for client validation");
        const char* username = std::getenv("PICO_TEST_USER");
        const char* key = std::getenv("PICO_TEST_KEY");
        require(username && key, "Missing disposable test account");
        pico_page::sync::Config config{"https://loopback.invalid", username, key, "pico-validation", 20260812};
        pico_page::ReadingRecord saved{document.metadata().book_id, "epub-native-v1", "pico-validation", 7, anchor};
        pico_page::sync::HttpRequest request;
        require(pico_page::sync::build_put_request(config, document.metadata().partial_md5, pointer, .777, saved, request, error), error);
        string(output, "method", request.method);
        string(output, "body", request.body);
        auto* headers = cJSON_AddObjectToObject(output, "headers");
        for (const auto& header : request.headers) string(headers, header.first.c_str(), header.second);
        require(request.uploaded_record && request.uploaded_record->local_revision == 7, "Upload revision changed");
    } else {
        const std::string body{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
        const auto remote = pico_page::sync::parse_remote(200, body);
        require(remote.status == pico_page::sync::RemoteStatus::Found, remote.error);
        require(pico_page::sync::from_xpointer(document, remote.progress, 20260812, anchor, error), error);
        std::size_t index = 0;
        require(document.chapter_index_for(anchor.resource, index) && document.load_chapter(index, chapter, error), error);
        require(pico_page::sync::to_xpointer(document, chapter, anchor, 20260812, pointer, error), error);
        std::uint32_t pos = 0;
        require(chapter.offset_for(anchor, pos), "Recovered anchor was not retained");
        std::uint32_t length = 24;
        pico_page::Anchor end;
        std::string end_pointer;
        while (length && (pos + length >= chapter.text.size() || !chapter.anchor_at(pos + length, end) ||
               end.node != anchor.node || end.offset != anchor.offset + length ||
               !pico_page::sync::to_xpointer(document, chapter, end, 20260812, end_pointer, error))) --length;
        require(length > 0, "Client test position has no comparable retained text range");
        string(output, "text", utf8(chapter.text.substr(pos, length)));
        string(output, "end_pointer", end_pointer);
    }
    string(output, "pointer", pointer);
    string(output, "resource", anchor.resource);
    string(output, "node", anchor.node);
    cJSON_AddNumberToObject(output, "offset", anchor.offset);
    auto* serialized = cJSON_PrintUnformatted(output);
    require(serialized != nullptr, "JSON allocation failed");
    std::cout << serialized << '\n';
    cJSON_free(serialized);
    cJSON_Delete(output);
}
