#include "pico_page/dictionary.h"
#include "pico_page/dictionary_text.h"
#include "pico_page/document.h"
#include "pico_page/store.h"
#include <chrono>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    using namespace pico_page::dictionary;
    int file_argument = 1;
    std::unique_ptr<pico_page::FileStore> store;
    if (argc > 2 && std::string(argv[1]) == "--cache") {
        store = std::make_unique<pico_page::FileStore>(std::filesystem::absolute(argv[2]).lexically_normal().string());
        file_argument = 3;
    }
    const bool decode_text = argc > file_argument && std::string(argv[file_argument]) == "--text";
    if (decode_text) ++file_argument;
    if (argc <= file_argument + 1) { std::cerr << "Usage: dictionary_probe [--cache card-root] [--text] dictionary.ifo word [word ...]\n"; return 2; }
    Verification verification;
    const auto start = std::chrono::steady_clock::now();
    Dictionary dictionary;
    auto result = store ? dictionary.open_prepared(argv[file_argument], *store, verification) : dictionary.open(argv[file_argument]);
    if (!result) { std::cerr << result.message << '\n'; return 1; }
    const auto prepared = std::chrono::steady_clock::now();
    std::cout << "words=" << dictionary.info().words << " synonyms=" << dictionary.info().synonyms
              << " reused_index=" << dictionary.reused_index() << " prepare_ms=" << std::chrono::duration<double, std::milli>(prepared - start).count() << '\n';
    if (store) {
        const auto warm = std::chrono::steady_clock::now();
        result = dictionary.open_prepared(argv[file_argument], *store, verification);
        if (!result) { std::cerr << result.message << '\n'; return 1; }
        std::cout << "warm_reused_index=" << dictionary.reused_index()
                  << " warm_open_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - warm).count() << '\n';
    }
    unsigned found = 0;
    for (int i = file_argument + 1; i < argc; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        std::uint32_t next = 0;
        bool more = false;
        unsigned definitions = 0;
        std::size_t bytes = 0, scalars = 0;
        bool degraded = false, omitted_fields = false;
        std::string canonical, headwords;
        do {
            std::vector<Entry> entries;
            result = dictionary.lookup(argv[i], entries, more, next);
            if (!result) { std::cerr << "query=" << i - file_argument - 1 << " status=" << static_cast<int>(result.status) << ' ' << result.message << '\n'; return 1; }
            for (const auto& entry : entries) {
                for (int shift : {24, 16, 8, 0}) headwords += static_cast<char>((entry.headword.size() >> shift) & 255);
                headwords += entry.headword;
                std::vector<Field> fields;
                result = dictionary.read(entry.ordinal, fields);
                if (!result) { std::cerr << "query=" << i - file_argument - 1 << ' ' << result.message << '\n'; return 1; }
                omitted_fields = omitted_fields || result.omitted_fields;
                for (const auto& field : fields) {
                    bytes += field.text.size();
                    canonical += field.type;
                    for (int shift : {24, 16, 8, 0}) canonical += static_cast<char>((field.text.size() >> shift) & 255);
                    canonical += field.text;
                }
                if (decode_text) {
                    TextCursor cursor;
                    for (;;) {
                        TextChunk chunk;
                        result = text_chunk(fields, cursor, 4096, chunk);
                        if (!result) { std::cerr << "query=" << i - file_argument - 1 << " text: " << result.message << '\n'; return 1; }
                        scalars += chunk.text.size();
                        if (chunk.end) break;
                    }
                    degraded = degraded || cursor.degraded;
                }
                ++definitions;
            }
            next = entries.back().ordinal + 1;
        } while (more);
        std::cout << "query=" << i - file_argument - 1 << " definitions=" << definitions << " text_bytes=" << bytes
                  << " text_scalars=" << scalars << " degraded=" << degraded
                  << " omitted_fields=" << omitted_fields
                  << " field_digest=" << pico_page::md5_hex(canonical)
                  << " headword_digest=" << pico_page::md5_hex(headwords)
                  << " lookup_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() << '\n';
        ++found;
    }
    std::cout << "queries_passed=" << found << '\n';
}
