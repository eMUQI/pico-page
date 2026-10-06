#include "pico_page/core.h"
#include "pico_page/store.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <unistd.h>

using namespace pico_page;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #__VA_ARGS__ "\n"; std::exit(1); } } while (false)

struct RecordingDisplay final : DisplayBackend {
    std::vector<Frame> submitted;
    bool start(const Frame& frame) override { submitted.push_back(frame); return true; }
};

int main() {
    std::string pattern = (std::filesystem::temp_directory_path() / "pico-page-flow-XXXXXX").string();
    char* directory = mkdtemp(pattern.data());
    CHECK(directory);
    const std::filesystem::path root(directory);
    std::filesystem::create_directories(root / "Books");
    std::ofstream(root / "Books/synthetic.epub") << "synthetic source for session integration; not an EPUB parser fixture";
    FileStore store(root.string());
    RecordingDisplay recording;
    Display display(recording);
    Session session(display);
    InputRouter inputs;
    Context context = Context::Reading;
    session.open("synthetic-identity", "scalar-source-v1", "host-demo");
    auto first = session.request_page({"chapter.xhtml", "paragraph-1", 0});
    CHECK(session.layout_ready(first));
    CHECK(!session.request_save());
    CHECK(session.presented(*display.finish(first, true)));
    auto save = session.request_save();
    CHECK(save);
    auto committed = store.save_progress("Books/synthetic.epub", save->record);
    CHECK(committed.status == StoreStatus::Ok && committed.record == std::optional<ReadingRecord>(save->record));
    CHECK(session.saved(*save, committed.status == StoreStatus::Ok));

    auto dispatch = [&](InputEvent event) {
        auto action = inputs.route(context, event);
        switch (action) {
        case Action::OpenTools: context = Context::Tools; break;
        case Action::Close: context = Context::Reading; break;
        case Action::ForceRefresh: session.force_refresh(); break;
        case Action::NextPage: {
            auto token = session.request_page({"chapter.xhtml", "paragraph-1", 100});
            CHECK(session.layout_ready(token));
            break;
        }
        default: break;
        }
    };
    dispatch({1, Phase::Begin, Input::K3});
    dispatch({1, Phase::End, Input::K3});
    CHECK(session.visible_record()->anchor.offset == 0);
    auto token = recording.submitted.back().token;
    CHECK(!session.presented(*display.finish(token, false)));
    CHECK(!session.needs_save());
    dispatch({2, Phase::Begin, Input::K3});
    dispatch({2, Phase::End, Input::K3});
    token = recording.submitted.back().token;
    CHECK(session.presented(*display.finish(token, true)));
    save = session.request_save();
    CHECK(save && save->record.anchor.offset == 100);
    committed = store.save_progress("Books/synthetic.epub", save->record);
    CHECK(committed.status == StoreStatus::Ok);
    CHECK(session.saved(*save, true));
    auto before_refresh = *session.visible_record();
    const auto before_count = recording.submitted.size();
    dispatch({3, Phase::Begin, Input::K2});
    dispatch({3, Phase::End, Input::K2});
    dispatch({4, Phase::Begin, Input::K2});
    dispatch({4, Phase::End, Input::K2});
    CHECK(recording.submitted.size() == before_count + 1);
    CHECK(recording.submitted.back().refresh == RefreshIntent::FullClean);
    CHECK(!session.presented(*display.finish(recording.submitted.back().token, true)));
    CHECK(*session.visible_record() == before_refresh && !session.needs_save());
    dispatch({5, Phase::Begin, Input::TouchCenter});
    dispatch({5, Phase::End, Input::TouchCenter});
    CHECK(context == Context::Tools);
    dispatch({6, Phase::Begin, Input::K2});
    dispatch({6, Phase::End, Input::K2});
    CHECK(context == Context::Tools && recording.submitted.size() == before_count + 2);
    CHECK(!session.presented(*display.finish(recording.submitted.back().token, true)));
    CHECK(*session.visible_record() == before_refresh && !session.needs_save());

    std::filesystem::create_directories(root / "Books/synthetic.epub.picopage/prepared");
    std::filesystem::create_directories(root / "PicoPage/cache/layouts");
    std::ofstream(root / "Books/synthetic.epub.picopage/prepared/required.txt") << "required source mapping";
    std::ofstream(root / "PicoPage/cache/layouts/page") << "rebuildable";
    const std::string invalid_settings = "schema_version = 1\n[reader\n";
    std::ofstream(root / "PicoPage/settings.toml") << invalid_settings;
    CHECK(!store.load_settings().diagnostics.empty());
    std::ifstream preserved(root / "PicoPage/settings.toml");
    CHECK(std::string(std::istreambuf_iterator<char>(preserved), {}) == invalid_settings);
    CHECK(store.clear_cache() == StoreStatus::Ok);
    CHECK(std::filesystem::exists(root / "Books/synthetic.epub.picopage/prepared/required.txt"));
    CHECK(std::filesystem::exists(root / "Books/synthetic.epub"));

    FileStore reopened(root.string());
    auto restored = reopened.load_progress("Books/synthetic.epub", "synthetic-identity", "scalar-source-v1");
    CHECK(restored.status == StoreStatus::Ok && restored.record && *restored.record == before_refresh);
    Session next_session(display);
    next_session.open("synthetic-identity", "scalar-source-v1", "host-demo", restored.record);
    CHECK(!next_session.visible_record());
    token = next_session.request_page(restored.record->anchor);
    CHECK(next_session.layout_ready(token));
    CHECK(next_session.presented(*display.finish(token, true)));
    CHECK(next_session.visible_record()->anchor.offset == 100);
    CHECK(*next_session.visible_record() == *restored.record && !next_session.needs_save());
    CHECK(reopened.load_progress("Books/synthetic.epub", "different-content", "scalar-source-v1").status == StoreStatus::IdentityMismatch);
    std::filesystem::remove_all(root);
    std::cout << "PASS: input -> session -> recording display -> file commit -> reopen (host only)\n";
}
