#include "pico_page/dictionary.h"
#include "pico_page/dictionary_selection.h"
#include "pico_page/dictionary_text.h"
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include "pico_page/store.h"
#include "pico_page/document.h"
#include <iostream>

using namespace pico_page::dictionary;
namespace {
void text_checks() {
    const std::vector<Field> fields{{'m', "plain <literal>\r\n"}, {'t', "fəˈnetɪk"},
        {'h', "<b>Bold &amp; &#x20000;</b><p>Example &eacute; &unknown;</p><ul><li>one<li>two</ul><script>if (a<b) x='>'; </script><style>p { color:red; }</style>end"},
        {'g', "<span foreground='blue'>色彩 <i>强调</i></span>\n第二义项\n  例句"}, {'x', "<k>词头</k><def>释义</def>"}};
    TextCursor cursor;
    TextChunk full;
    assert(text_chunk(fields, cursor, 4096, full) && full.end && cursor.degraded);
    assert(full.text.find(U"plain <literal>\n") == 0);
    assert(full.text.find(U"Bold & 𠀀\nExample é &unknown;") != std::u32string::npos);
    assert(full.text.find(U"• one\n• two\nend") != std::u32string::npos);
    assert(full.text.find(U"if (a") == std::u32string::npos && full.text.find(U"color:red") == std::u32string::npos);
    assert(full.text.find(U"色彩 强调\n第二义项\n  例句\n词头\n释义") != std::u32string::npos);
    const auto bold = full.text.find(U"Bold"); assert(full.emphasis[bold] && !full.emphasis[0]);
    TextCursor streamed;
    std::u32string text;
    std::vector<bool> emphasis;
    for (;;) {
        TextChunk chunk; assert(text_chunk(fields, streamed, 3, chunk));
        text += chunk.text; emphasis.insert(emphasis.end(), chunk.emphasis.begin(), chunk.emphasis.end());
        if (chunk.end) break;
    }
    assert(text == full.text && emphasis == full.emphasis && streamed.degraded);
    assert(streamed.scalars == text.size() && cursor.scalars == full.text.size());
    for (const auto* bad : {"<b", "<b x='unterminated>", "<!-- comment", "<script>not closed", "&#xD800;", "&#0;"}) {
        TextCursor position; TextChunk chunk;
        assert(!text_chunk({{'h', bad}}, position, 4096, chunk));
        assert(position.field == 0 && position.byte == 0 && chunk.text.empty());
    }
    TextCursor literal_cursor; TextChunk literal_chunk;
    const std::string ampersands(4096, '&');
    assert(text_chunk({{'h', ampersands}}, literal_cursor, 4096, literal_chunk));
    assert(literal_chunk.text == std::u32string(4096, U'&') && literal_cursor.degraded);
    TextCursor position; TextChunk chunk;
    std::string nested;
    for (int i = 0; i < 65; ++i) nested += "<span>";
    assert(text_chunk({{'h', nested}}, position, 4096, chunk).status == Status::ResourceLimit);
    unsigned checks = 0;
    assert(text_chunk({{'h', "<!--" + std::string(4000, 'x') + "-->end"}}, position, 4096, chunk,
        [&] { return ++checks > 4; }).status == Status::Cancelled);
    assert(position.byte == 0 && chunk.text.empty());
}
void selection_checks() {
    Selection selection;
    std::vector<Candidate> candidates;
    const auto has = [&](std::string_view word, bool inflection) {
        return std::any_of(candidates.begin(), candidates.end(), [&](const Candidate& item) {
            return item.word == word && item.inflection == inflection;
        });
    };
    const std::u32string chinese = U"前文。中华人民共和国万岁。后文";
    const Selection visible{3, 12};
    assert(initial_selection(chinese, visible, 7, selection));
    assert(selection.begin == 7 && selection.end == 8);
    assert(selection_candidates(chinese, visible, selection, 7, false, candidates));
    assert(has("中华人民共和国", false));
    std::size_t previous = 9;
    for (const auto& item : candidates) {
        assert(item.selection.begin >= visible.begin && item.selection.end <= visible.end);
        assert(item.selection.begin <= 7 && item.selection.end > 7);
        assert(item.selection.end - item.selection.begin <= previous);
        previous = item.selection.end - item.selection.begin;
    }
    assert(candidates.back().word == "共" && candidates.size() <= 36);
    assert(selection_candidates(chinese, visible, {5, 9}, 1000, true, candidates));
    assert(candidates.size() == 1 && candidates[0].word == "人民共和");
    const std::u32string extended = U"一二三四五六七八九十百千万亿兆京垓";
    assert(initial_selection(extended, {0, extended.size()}, 8, selection));
    assert(selection_candidates(extended, {0, extended.size()}, selection, 8, false, candidates));
    assert(candidates.size() == 36 && candidates.front().selection.end - candidates.front().selection.begin == 8);
    for (const auto& word : {U"Running", U"studies", U"tried", U"making", U"reader’s", U"co\u00adoperate", U"CAFÉ"}) {
        const std::u32string text = std::u32string(U"“") + word + U"”。next";
        assert(initial_selection(text, {0, text.size()}, 2, selection));
        assert(selection.begin == 1 && selection.end == 1 + std::u32string_view(word).size());
        assert(selection_candidates(text, {0, text.size()}, selection, 2, false, candidates));
        assert(candidates.size() <= 36);
        if (std::u32string_view(word) == U"Running") assert(has("Running", false) && has("run", true));
        if (std::u32string_view(word) == U"studies") assert(has("study", true));
        if (std::u32string_view(word) == U"tried") assert(has("try", true));
        if (std::u32string_view(word) == U"making") assert(has("make", true));
        if (std::u32string_view(word) == U"reader’s") assert(has("reader's", false) && has("reader", true));
        if (std::u32string_view(word) == U"co\u00adoperate") assert(has("cooperate", false));
        if (std::u32string_view(word) == U"CAFÉ") assert(candidates.front().word == "CAFÉ");
        bool inflected = false;
        for (const auto& item : candidates) {
            assert(!inflected || item.inflection);
            inflected = inflected || item.inflection;
        }
    }
    assert(initial_selection(U"first\nsecond", {0, 12}, 7, selection));
    assert(selection.begin == 6 && selection.end == 12);
    assert(initial_selection(U"pageboundary", {4, 8}, 6, selection));
    assert(selection.begin == 4 && selection.end == 8);
    assert(initial_selection(U"hello world", {0, 11}, 5, selection).status == Status::NotFound);
    assert(!initial_selection(U"hello", {0, 6}, 0, selection));
    assert(!initial_selection(U"hello", {0, 5}, 5, selection));
    const std::u32string long_word(65, U'a');
    assert(initial_selection(long_word, {0, 65}, 32, selection).status == Status::ResourceLimit);
    assert(selection_candidates(long_word, {0, 65}, {0, 65}, 0, true, candidates).status == Status::ResourceLimit);
    assert(candidates.empty());
    const std::u32string wide(64, U'\U00020000');
    assert(selection_candidates(wide, {0, 64}, {0, 64}, 0, true, candidates).status == Status::ResourceLimit);
    assert(candidates.empty());
    assert(!selection_candidates(U"valid", {0, 5}, {4, 3}, 0, true, candidates) && candidates.empty());
    assert(!selection_candidates(U"valid", {0, 5}, {1, 3}, 4, false, candidates) && candidates.empty());
    const std::u32string invalid(1, static_cast<char32_t>(0xd800));
    assert(!selection_candidates(invalid, {0, 1}, {0, 1}, 0, true, candidates) && candidates.empty());
}
}  // namespace
int main(int argc, char** argv) {
    text_checks();
    selection_checks();
    assert(argc == 2);
    const std::string root = argv[1];
    Dictionary dictionary;
    std::vector<Entry> entries;
    std::vector<Field> fields;
    bool more = false;
    auto open = [&](const char* name) { return dictionary.open(root + '/' + name + ".ifo"); };
    auto found = [&](const char* word) {
        const auto result = dictionary.lookup(word, entries, more);
        if (!result) std::cerr << result.message << '\n';
        assert(result && !entries.empty());
    };
    auto read = [&](std::uint32_t ordinal) {
        const auto result = dictionary.read(ordinal, fields);
        if (!result) std::cerr << result.message << '\n';
        assert(result);
        return result;
    };
    Info metadata;
    assert(inspect(root + "/compressed.ifo", metadata) && metadata.words == 605);
    assert(inspect(root + "/packed.ifo.gz", metadata) && metadata.words == 2);
    assert(!inspect(root + "/missing.ifo", metadata) && metadata.name.empty());
    assert(inspect(root + "/long-name.ifo", metadata).status == Status::ResourceLimit && metadata.name.empty());
    assert(inspect(root + "/compressed.ifo", metadata, [] { return true; }).status == Status::Cancelled);
    assert(compare("APPLE", "Apple") < 0 && compare("apple", "Banana") < 0);
    assert(open("compressed"));
    assert(dictionary.info().words == 605 && dictionary.info().synonyms == 4);
    found("apple"); assert(entries.size() == 1 && entries[0].ordinal == 2);
    read(2); assert(fields.size() == 1 && fields[0].text == "lower <literal>");
    found("alias"); assert(entries.size() == 2 && entries[0].ordinal == 0 && entries[1].ordinal == 2);
    assert(entries[0].synonym && entries[1].headword == "apple");
    found("别名"); read(entries[0].ordinal); assert(fields[0].text == "词条释义");
    found("omega"); read(entries[0].ordinal); assert(fields[0].text.size() == 4205);
    unsigned count = 0;
    std::uint32_t next = 0;
    do {
        assert(dictionary.lookup("duplicate", entries, more, next));
        for (const auto& entry : entries) { assert(entry.ordinal == count + 3); ++count; }
        next = entries.back().ordinal + 1;
    } while (more);
    assert(count == 600);
    assert(dictionary.lookup("missing", entries, more).status == Status::NotFound && entries.empty());
    assert(dictionary.lookup("apple", entries, more, 0, [] { return true; }).status == Status::Cancelled);
    assert(dictionary.read(2, fields, [] { return true; }).status == Status::Cancelled && fields.empty());
    assert(open("repetitive")); read(0); assert(fields[0].text == std::string(100000, 'a'));
    assert(open("plain")); found("apple"); read(0); assert(fields[0].text == "lower <literal>");
    assert(open("fields")); assert(read(0).omitted_fields); assert(fields.size() == 5 && fields[0].type == 't' && fields[4].type == 'x');
    assert(open("tm")); assert(!read(0).omitted_fields); assert(fields.size() == 2 && fields[0].text == "音标" && fields[1].text == "definition");
    assert(open("empty-last")); read(0); assert(fields.size() == 2 && fields[1].text.empty());
    assert(open("binary-last")); assert(read(0).omitted_fields); assert(fields.size() == 1 && fields[0].text == "text");
    assert(open("unknown-text")); assert(read(0).omitted_fields); assert(fields.size() == 1 && fields[0].text == "text");
    assert(open("unsupported-only")); assert(dictionary.read(0, fields).status == Status::Unsupported && fields.empty());
    assert(open("invalid-field")); assert(dictionary.read(0, fields).status == Status::InvalidFormat && fields.empty());
    assert(open("invalid-utf8")); assert(dictionary.read(0, fields).status == Status::InvalidFormat);
    assert(open("oversized")); assert(dictionary.read(0, fields).status == Status::ResourceLimit);
    for (const auto* name : {"unsorted", "bad-range", "bad-size", "bad-ra", "truncated"}) {
        const auto result = open(name);
        if (result) std::cerr << "Unexpected success: " << name << '\n';
        assert(result.status == Status::InvalidFormat);
        assert(!dictionary.lookup("apple", entries, more));
    }
    assert(open("offset64").status == Status::Unsupported);
    assert(open("gzip").status == Status::Unsupported);
    assert(open("bad-block")); assert(dictionary.read(0, fields).status == Status::InvalidFormat);
    unsigned steps = 0;
    assert(dictionary.open(root + "/compressed.ifo", [&] { return ++steps > 15; }).status == Status::Cancelled);
    assert(!dictionary.lookup("apple", entries, more));
    assert(open("compressed")); found("apple"); read(entries[0].ordinal);
    dictionary.close(); assert(!dictionary.read(0, fields));
    namespace fs = std::filesystem;
    const auto card = root + "/cache-card";
    fs::remove_all(card); fs::create_directories(card);
    pico_page::FileStore store(card);
    Verification verified;
    const auto prepared = [&](const std::string& name) { return dictionary.open_prepared(root + '/' + name, store, verified); };
    assert(prepared("compressed.ifo") && !dictionary.reused_index());
    assert(prepared("compressed.ifo") && dictionary.reused_index());
    found("alias"); read(entries[0].ordinal); assert(fields[0].text == "upper");
    verified.clear(); assert(prepared("compressed.ifo") && dictionary.reused_index());
    dictionary.close(); assert(store.clear_cache() == pico_page::StoreStatus::Ok);
    assert(prepared("compressed.ifo") && !dictionary.reused_index());
    verified.clear();
    assert(prepared("packed.ifo.gz") && !dictionary.reused_index());
    found("banana"); read(entries[0].ordinal); assert(fields[0].text == "fruit");
    assert(prepared("packed.ifo.gz") && dictionary.reused_index());
    dictionary.close(); verified.clear();
    const auto cache_dir = fs::path(card) / "PicoPage/cache/dictionaries" / pico_page::md5_hex(root + "/packed.ifo.gz");
    { std::fstream f(cache_dir / "index.raw", std::ios::in | std::ios::out | std::ios::binary); assert(f); f.put('z'); }
    assert(prepared("packed.ifo.gz") && !dictionary.reused_index());
    found("apple"); read(entries[0].ordinal); assert(fields[0].text == "word");
    dictionary.close(); verified.clear();
    { std::ofstream f(cache_dir / "index.bin", std::ios::binary); f << "broken"; }
    assert(prepared("packed.ifo.gz") && !dictionary.reused_index());
    verified.clear(); assert(prepared("packed-large.ifo"));
    found("word04095"); assert(entries[0].ordinal == 4095); read(entries[0].ordinal); assert(fields[0].text == "definition");
    assert(prepared("packed-large.ifo") && dictionary.reused_index());
    dictionary.close(); verified.clear(); assert(store.clear_cache() == pico_page::StoreStatus::Ok);
    const auto large_temporary = fs::path(card) / "PicoPage/cache/dictionaries" /
        pico_page::md5_hex(root + "/packed-large.ifo") / "index.raw.tmp";
    assert(dictionary.open_prepared(root + "/packed-large.ifo", store, verified, [&] {
        std::error_code error;
        const auto size = fs::file_size(large_temporary, error);
        return !error && size > 0;
    }).status == Status::Cancelled);
    assert(!fs::exists(large_temporary));
    assert(prepared("packed-large.ifo") && !dictionary.reused_index());
    for (const auto* name : {"gzip-crc.ifo", "gzip-truncated.ifo", "gzip-multi.ifo"}) {
        verified.clear(); const auto result = prepared(name);
        if (result || (result.status != Status::InvalidFormat && result.status != Status::Unsupported))
            std::cerr << name << ": " << static_cast<int>(result.status) << " " << result.message << '\n';
        assert(!result && (result.status == Status::InvalidFormat || result.status == Status::Unsupported));
    }
    verified.clear();
    unsigned callbacks = 0;
    assert(dictionary.open_prepared(root + "/packed.ifo.gz", store, verified,
        [&] { return ++callbacks > 8; }).status == Status::Cancelled);
    assert(prepared("packed.ifo.gz"));
    dictionary.close(); verified.clear();
    // Source replacement keeps its size but must invalidate the previous mount's cache.
    const auto replace_path = fs::path(root) / "plain.idx";
    { std::fstream f(replace_path, std::ios::in | std::ios::out | std::ios::binary); f.put('b'); }
    assert(prepared("plain.ifo")); found("bpple");
    dictionary.close(); verified.clear();
    { std::fstream f(replace_path, std::ios::in | std::ios::out | std::ios::binary); f.put('a'); }
    assert(prepared("plain.ifo") && !dictionary.reused_index()); found("apple");
    dictionary.close(); verified.clear();
    assert(store.clear_cache() == pico_page::StoreStatus::Ok);
    const auto failed_cache = fs::path(card) / "PicoPage/cache/dictionaries" / pico_page::md5_hex(root + "/plain.ifo") / "index.bin.tmp";
    fs::create_directories(failed_cache);
    assert(prepared("plain.ifo").status == Status::IoError);
    assert(!dictionary.lookup("apple", entries, more));
    fs::remove_all(failed_cache);
    assert(prepared("plain.ifo"));
    dictionary.close(); fs::remove_all(card);
    std::cout << "Dictionary format, range, duplicate, synonym and cancellation checks passed\n";
}
