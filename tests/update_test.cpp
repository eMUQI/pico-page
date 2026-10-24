#include "pico_page/update.h"
#include <cassert>
#include <iostream>

using namespace pico_page;
using update::Offer;

static std::string manifest_json(const std::string& version, const std::string& extra = "",
                                 const std::string& notes = "## 新增\\n- **书签**：可添加。") {
    return "{\"manifest\":1,\"board\":\"read_pico\",\"version\":\"" + version +
           "\",\"published_at\":\"2026-10-18\",\"url\":\"https://picopage.wulu.zone/firmware/a.bin\","
           "\"size\":2936012,\"sha256\":\"" + std::string(64, 'A') + "\","
           "\"data_schema\":{\"progress\":" + std::to_string(update::progress_schema) + ",\"config\":" +
           std::to_string(update::config_schema) + "},\"notes\":\"" + notes + "\"" + extra + "}";
}

int main() {
    // The order given in docs/development/versioning.md; build metadata takes no part in it.
    const char* ascending[] = {"0.2.0-beta.1", "0.2.0-beta.2", "0.2.0-beta.10", "0.2.0-rc.1", "0.2.0", "0.2.1",
                               "0.3.0-beta.1", "0.10.0", "1.0.0"};
    for (std::size_t i = 0; i < std::size(ascending); ++i)
        for (std::size_t j = 0; j < std::size(ascending); ++j) {
            const auto order = update::compare_versions(ascending[i], ascending[j]);
            assert(order && *order == (i < j ? -1 : i > j ? 1 : 0));
        }
    assert(*update::compare_versions("0.2.0-beta.1+g1234567.dirty", "0.2.0-beta.1") == 0);
    for (const char* invalid : {"", "0.2", "0.2.0-alpha.1", "0.2.0-beta", "0.2.0-beta.0", "01.2.0", "0.2.0-rc.1x",
                                "v0.2.0", "0.2.0-beta.01", "0.2.0.1", "99999999999.0.0"})
        assert(!update::compare_versions(invalid, "0.2.0"));
    assert(update::development_build("0.2.0-beta.1+g1234567") && !update::development_build("0.2.0-beta.1"));
    assert(update::release_version("0.2.0-beta.1+g1234567.dirty") == "0.2.0-beta.1");

    update::Manifest manifest;
    std::string error;
    assert(update::parse_manifest(manifest_json("0.2.0-beta.2"), manifest, error) && error.empty());
    assert(manifest.version == "0.2.0-beta.2" && manifest.size == 2936012 && manifest.sha256 == std::string(64, 'a') &&
           manifest.published_at == "2026-10-18" && manifest.progress_schema == update::progress_schema &&
           manifest.config_schema == update::config_schema && !manifest.notes_truncated);
    // Anything the device cannot verify later is refused before it is cached or offered.
    for (const auto& bad : {std::string("[]"), std::string("{\"manifest\":2}"),
             std::string("{\"manifest\":1,\"board\":\"other\"}"), manifest_json("0.2.0-beta.2+g1234567"), manifest_json("0.2")})
        assert(!update::parse_manifest(bad, manifest, error) && !error.empty() && manifest.version.empty());
    auto altered = manifest_json("0.2.0");
    altered.replace(altered.find("https://"), 8, "http://x");
    assert(!update::parse_manifest(altered, manifest, error));
    altered = manifest_json("0.2.0");
    altered.replace(altered.find("\"size\":2936012"), 14, "\"size\":0");
    assert(!update::parse_manifest(altered, manifest, error));
    altered = manifest_json("0.2.0");
    altered.replace(altered.find(std::string(64, 'A')), 64, std::string(63, 'a') + "g");
    assert(!update::parse_manifest(altered, manifest, error));
    // Notes are cut on a scalar boundary; the three-byte scalar that straddles the limit is dropped.
    assert(update::parse_manifest(manifest_json("0.2.0", "", std::string(update::notes_limit - 1, 'a') + "书签"), manifest, error));
    assert(manifest.notes_truncated && manifest.notes.size() == update::notes_limit - 1);

    assert(update::parse_manifest(manifest_json("0.2.0-beta.2"), manifest, error));
    assert(update::evaluate(manifest, "0.2.0-beta.1", "") == Offer::Newer);
    assert(update::evaluate(manifest, "0.2.0-beta.2", "") == Offer::Current);
    assert(update::evaluate(manifest, "0.2.0", "") == Offer::Current);
    assert(update::evaluate(manifest, "0.2.0-beta.1", "0.2.0-beta.2") == Offer::Skipped);
    assert(update::evaluate(manifest, "0.2.0-beta.1", "0.2.0-beta.1") == Offer::Newer);
    assert(update::evaluate(manifest, "0.2.0-beta.3+g1234567", "") == Offer::Development);
    // A development build also withholds the exact version that failed to start.
    assert(update::evaluate(manifest, "0.2.0-beta.3+g1234567", "0.2.0-beta.2") == Offer::Skipped);
    assert(update::evaluate(manifest, "0.2.0-beta.3+g1234567", "0.2.0-beta.1") == Offer::Development);
    assert(update::evaluate(manifest, "unknown", "") == Offer::Development);
    for (int formats = 0; formats < 4; ++formats) {
        auto candidate = manifest;
        candidate.progress_schema += formats & 1;
        candidate.config_schema += (formats >> 1) & 1;
        assert(update::requires_schema_confirmation(candidate) == (formats != 0));
        assert(update::evaluate(candidate, "0.2.0-beta.1", "") == (formats ? Offer::NewerSchema : Offer::Newer));
        assert(update::evaluate(candidate, "0.2.0-beta.3+g1234567", "") == Offer::Development);
        assert(update::evaluate(candidate, "unknown", "") == Offer::Development);
    }
    auto changed = manifest;
    changed.progress_schema = update::progress_schema + 1;
    assert(update::evaluate(changed, "0.2.0-beta.1", "") == Offer::NewerSchema);
    changed.progress_schema = update::progress_schema; changed.config_schema = update::config_schema - 1;
    assert(update::evaluate(changed, "0.2.0-beta.1", "") == Offer::OlderSchema);
    // Cleared-progress markers use schema 2, so firmware that writes only schema 1 is older.
    changed = manifest; changed.progress_schema = 1;
    assert(update::progress_schema == 2 && update::evaluate(changed, "0.2.0-beta.1", "") == Offer::OlderSchema);
    // The hold names one version: another version that is newer than the running one is offered.
    assert(update::evaluate(manifest, "0.2.0-beta.1", "0.2.0-beta.3") == Offer::Newer);
    changed = manifest; changed.size = update::slot_bytes + 1;
    assert(update::evaluate(changed, "0.2.0-beta.1", "") == Offer::TooLarge);
    changed.size = update::slot_bytes;
    assert(update::evaluate(changed, "0.2.0-beta.1", "") == Offer::Newer);

    const auto blocks = update::parse_notes(
        "# 标题\r\n\n## 新增\n- **书签**：阅读页`工具`面板，见[说明](https://example.com/a)。\n   * 第二项\n"
        "####不是标题\n普通 **未闭合\n| 表 | 格 |\n");
    using Kind = update::NoteBlock::Kind;
    assert(blocks.size() == 7 && blocks[0].kind == Kind::Heading && blocks[0].spans[0].text == "标题");
    assert(blocks[1].kind == Kind::Heading && blocks[1].spans[0].text == "新增");
    assert(blocks[2].kind == Kind::Item && blocks[2].spans.size() == 2 && blocks[2].spans[0].bold &&
           blocks[2].spans[0].text == "书签" && !blocks[2].spans[1].bold &&
           blocks[2].spans[1].text == "：阅读页工具面板，见说明。");
    assert(blocks[3].kind == Kind::Item && blocks[3].spans[0].text == "第二项");
    assert(blocks[4].kind == Kind::Text && blocks[4].spans[0].text == "####不是标题");
    assert(blocks[5].kind == Kind::Text && blocks[5].spans[0].text == "普通 **未闭合" && !blocks[5].spans[0].bold);
    assert(blocks[6].spans[0].text == "| 表 | 格 |");
    assert(update::parse_notes("").empty() && update::parse_notes(" \n\n").empty());

    update::State state, loaded;
    state.manifest = manifest; state.checked_at = 1790000000; state.pending_from = "0.2.0-beta.1";
    state.pending_to = "0.2.0-beta.2"; state.skipped = "0.1.9";
    const auto encoded = update::encode_state(state);
    assert(!encoded.empty() && encoded.size() < update::state_limit && update::decode_state(encoded, loaded));
    assert(loaded.manifest && loaded.manifest->version == manifest.version && loaded.manifest->notes == manifest.notes &&
           loaded.manifest->sha256 == manifest.sha256 && loaded.manifest->size == manifest.size &&
           loaded.checked_at == state.checked_at && loaded.pending_from == state.pending_from &&
           loaded.pending_to == state.pending_to && loaded.skipped == state.skipped);
    assert(update::decode_state(update::encode_state({}), loaded) && !loaded.manifest && loaded.pending_to.empty());
    assert(!update::decode_state("{", loaded) && !update::decode_state("{\"checked_at\":1}", loaded));
    // A cached manifest that no longer passes validation invalidates the whole entry.
    auto broken = encoded;
    broken.replace(broken.find("https://"), 8, "http://x");
    assert(!update::decode_state(broken, loaded) && !loaded.manifest && loaded.skipped.empty());
    std::cout << "update tests passed\n";
}
