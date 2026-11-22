#include "pico_page/core.h"
#include "../firmware/components/platform_read_pico/touch_moves.h"
#include "../firmware/components/platform_read_pico/usb_block_range.h"
#include "pico_page/released_tap.h"
#include "pico_page/sleep_input.h"
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace pico_page;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #__VA_ARGS__ "\n"; std::exit(1); } } while (false)

struct Recorder final : DisplayBackend {
    std::vector<Frame> frames;
    bool reject = false;
    bool start(const Frame& frame) override {
        if (reject) return false;
        frames.push_back(frame);
        return true;
    }
};

int main() {
    {
        using namespace pico_page::platform;
        using namespace pico_page::input;
        SleepInput input;
        input.begin();
        for (const auto kind : {EventKind::Touch, EventKind::K1, EventKind::K2, EventKind::K3, EventKind::InputReset}) {
            CHECK(SleepInput::ignores(kind));
            for (const auto phase : {EventPhase::Begin, EventPhase::End, EventPhase::Cancel, EventPhase::Move}) {
                input.accept({kind, phase, 1, 300, 500, 1000});
                CHECK(input.active() && !input.menu() && !input.power());
            }
        }
        for (const auto kind : {EventKind::None, EventKind::Power, EventKind::PowerLong, EventKind::SdRemoved})
            CHECK(!SleepInput::ignores(kind));
        input.accept({EventKind::Power});
        CHECK(input.power());
        input.accept({EventKind::Touch, EventPhase::Begin, 2});
        input.accept({EventKind::InputReset});
        CHECK(input.active() && input.power());
        input.accept({EventKind::Power});
        CHECK(!input.power());
        input.accept({EventKind::PowerLong});
        input.accept({EventKind::Power});
        input.accept({EventKind::InputReset});
        input.accept({EventKind::K2, EventPhase::End, 3});
        CHECK(input.active() && input.menu() && !input.power());
        CHECK(!input.ready(true, false) && input.menu());
        CHECK(!input.ready(false, true) && input.menu());
        CHECK(input.ready(true, true) && input.menu());
        input.clear();
        CHECK(!input.active() && !input.menu() && !input.power());
        CHECK(!input.ready(true, true));
        input.accept({EventKind::PowerLong});
        CHECK(!input.menu());
    }
    {
        CHECK((RefreshRegion{0, 0, 684, 1216}.fits(684, 1216)));
        CHECK(!(RefreshRegion{-1, 0, 10, 10}.fits(684, 1216)));
        CHECK(!(RefreshRegion{0, 0, 0, 10}.fits(684, 1216)));
        CHECK(!(RefreshRegion{683, 1215, 2, 1}.fits(684, 1216)));
        CHECK(!(RefreshRegion{1, 1, INT32_MAX, INT32_MAX}.fits(684, 1216)));
        Recorder recorder;
        Display screen(recorder);
        Frame patch{{"image", 1, 1, 1}, {}, FramePurpose::Overlay, RefreshIntent::Content,
                    RefreshRegion{36, 100, 612, 480}};
        CHECK(screen.present(patch));
        CHECK(recorder.frames.back().refresh == RefreshIntent::FullClean && !recorder.frames.back().region);
        CHECK(!recorder.frames.back().changed_pixels_only);
        CHECK(screen.finish(patch.token, true));
        ++patch.token.request;
        CHECK(screen.present(patch));
        CHECK(recorder.frames.back().region && recorder.frames.back().refresh == RefreshIntent::Content);
        CHECK(recorder.frames.back().changed_pixels_only);
        CHECK(screen.finish(patch.token, true));
        ++patch.token.request;
        const auto region = patch.region;
        patch.region.reset();
        CHECK(screen.present(patch));
        CHECK(recorder.frames.back().refresh == RefreshIntent::Content && !recorder.frames.back().region);
        CHECK(recorder.frames.back().changed_pixels_only);
        CHECK(screen.finish(patch.token, true));
        ++patch.token.request;
        patch.purpose = FramePurpose::Body;
        patch.changed_pixels_only = true;
        CHECK(screen.present(patch));
        CHECK(recorder.frames.back().refresh == RefreshIntent::Content);
        CHECK(!recorder.frames.back().changed_pixels_only);
        CHECK(screen.finish(patch.token, true));
        ++patch.token.request;
        patch.purpose = FramePurpose::Overlay;
        patch.refresh = RefreshIntent::FullClean;
        patch.region = region;
        CHECK(screen.present(patch));
        CHECK(recorder.frames.back().refresh == RefreshIntent::FullClean && !recorder.frames.back().region);
        CHECK(!recorder.frames.back().changed_pixels_only);
        CHECK(screen.finish(patch.token, true));
        ++patch.token.request;
        patch.refresh = RefreshIntent::Content;
        patch.changed_pixels_only = false;
        CHECK(screen.present(patch));
        CHECK(recorder.frames.back().region && recorder.frames.back().changed_pixels_only);
        CHECK(screen.finish(patch.token, false));
        ++patch.token.request;
        CHECK(screen.present(patch));
        CHECK(recorder.frames.back().refresh == RefreshIntent::FullClean && !recorder.frames.back().region);
        CHECK(!recorder.frames.back().changed_pixels_only);
        CHECK(screen.finish(patch.token, true));
        ++patch.token.request;
        patch.purpose = FramePurpose::Overlay;
        recorder.reject = true;
        CHECK(!screen.present(patch) && !screen.baseline_valid());
        recorder.reject = false;
        CHECK(screen.present(patch));
        CHECK(recorder.frames.back().refresh == RefreshIntent::FullClean && !recorder.frames.back().region);
        CHECK(!recorder.frames.back().changed_pixels_only);
        CHECK(screen.finish(patch.token, true));
        ++patch.token.request;
        patch.purpose = FramePurpose::ManualRefresh;
        CHECK(screen.present(patch));
        CHECK(recorder.frames.back().refresh == RefreshIntent::FullClean && !recorder.frames.back().region);
        CHECK(!recorder.frames.back().changed_pixels_only);
    }
    CHECK(usb_block_range(8, 0, 0, 512));
    CHECK(usb_block_range(8, 1, 511, 2));
    CHECK(usb_block_range(8, 7, 511, 1));
    CHECK(!usb_block_range(8, 7, 511, 2));
    CHECK(!usb_block_range(8, UINT32_MAX, UINT32_MAX, UINT32_MAX));
    CHECK(!usb_block_range(8, 8, 0, 1));
    {
        using namespace pico_page::platform;
        using namespace pico_page::input;
        ReleasedTap taps;
        CHECK(!taps.accept({EventKind::Touch, EventPhase::Begin, 1, 600, 500}, 1));
        CHECK(!taps.accept({EventKind::Touch, EventPhase::Begin, 1, 300, 500}, 1));
        const auto first_tap = taps.accept({EventKind::Touch, EventPhase::End, 1, 300, 500}, 1);
        CHECK(first_tap && first_tap->x == 600 && first_tap->phase == EventPhase::Begin);
        CHECK(!taps.accept({EventKind::Touch, EventPhase::End, 1}, 1));
        CHECK(!taps.accept({EventKind::K3, EventPhase::Begin, 2}, 1));
        CHECK(!taps.accept({EventKind::K3, EventPhase::End, 1}, 1));
        CHECK(taps.accept({EventKind::K3, EventPhase::End, 2}, 1));
        CHECK(!taps.accept({EventKind::Touch, EventPhase::Begin, 3, 600, 500}, 1));
        CHECK(!taps.accept({EventKind::Touch, EventPhase::Cancel, 3}, 1));
        CHECK(!taps.accept({EventKind::Touch, EventPhase::End, 3}, 1));
        CHECK(!taps.accept({EventKind::K2, EventPhase::Begin, 4}, 1));
        CHECK(taps.accept({EventKind::K2, EventPhase::End, 4}, 1));
        CHECK(!taps.accept({EventKind::Touch, EventPhase::Begin, 5, 600, 500}, 1));
        CHECK(!taps.accept({EventKind::Touch, EventPhase::End, 5, 600, 500}, 2));
        CHECK(!taps.accept({EventKind::K3, EventPhase::Begin, 6}, 2));
        CHECK(taps.accept({EventKind::K3, EventPhase::End, 6}, 2));
    }
    {
        using namespace pico_page::platform;
        using namespace pico_page::input;
        CHECK(long_press({EventKind::Touch, EventPhase::Begin, 1, 0, 0, 1000}, {EventKind::Touch, EventPhase::End, 1, 0, 0, 1000 + kLongPressUs}));
        CHECK(!long_press({EventKind::Touch, EventPhase::Begin, 1, 0, 0, 1000}, {EventKind::Touch, EventPhase::End, 1, 0, 0, 999 + kLongPressUs}));
        CHECK(!long_press({EventKind::K2, EventPhase::Begin, 1, 0, 0, 0}, {EventKind::K2, EventPhase::End, 1, 0, 0, 2 * kLongPressUs}));
        ReleasedTap taps;
        CHECK(!taps.accept({EventKind::Touch, EventPhase::Begin, 7, 300, 400, 1000}, 2));
        CHECK(!taps.due(999 + kLongPressUs, 2));
        CHECK(!taps.due(1000 + kLongPressUs, 3));
        const auto held = taps.due(1000 + kLongPressUs, 2);
        CHECK(held && held->sequence == 7 && held->x == 300);
        CHECK(taps.accept({EventKind::Touch, EventPhase::End, 7, 300, 400, 1000 + kLongPressUs}, 2));
        CHECK(!taps.due(1001 + kLongPressUs, 2));
        CHECK(!taps.accept({EventKind::Touch, EventPhase::End, 7, 300, 400, 1002 + kLongPressUs}, 2));
    }
    {
        using namespace pico_page::platform;
        using namespace pico_page::input;
        const Event begin{EventKind::Touch, EventPhase::Begin, 1, 300, 400, 1000};
        const auto classify = [&](int dx, int dy, int ms) {
            return released_gesture(begin, {EventKind::Touch, EventPhase::End, 1,
                                           300 + dx, 400 + dy, 1000 + ms * 1000});
        };
        CHECK(classify(59, 59, 900) == ReleasedGesture::Tap);
        CHECK(classify(-59, -59, 100) == ReleasedGesture::Tap);
        CHECK(classify(60, 0, 700) == ReleasedGesture::Previous);
        CHECK(classify(-60, 0, 700) == ReleasedGesture::Next);
        CHECK(classify(60, 0, 701) == ReleasedGesture::Ignore);
        CHECK(classify(173, 100, 700) == ReleasedGesture::Previous);
        CHECK(classify(172, 100, 700) == ReleasedGesture::Ignore);
        CHECK(classify(-173, -100, 700) == ReleasedGesture::Next);
        CHECK(classify(0, 60, 100) == ReleasedGesture::Ignore);
        CHECK(classify(60, 60, 100) == ReleasedGesture::Ignore);
        CHECK(classify(0, 0, -1) == ReleasedGesture::Ignore);
        for (const int sign : {-1, 1}) {
            CHECK(long_press(begin, {EventKind::Touch, EventPhase::End, 1, 300 + sign * 28, 400, 601000}));
            CHECK(!long_press(begin, {EventKind::Touch, EventPhase::End, 1, 300 + sign * 29, 400, 601000}));
            CHECK(long_press(begin, {EventKind::Touch, EventPhase::End, 1, 300, 400 + sign * 28, 601000}));
            CHECK(!long_press(begin, {EventKind::Touch, EventPhase::End, 1, 300, 400 + sign * 29, 601000}));
        }
        Event returned{EventKind::Touch, EventPhase::End, 1, 300, 400, 601000};
        returned.max_x = 329;
        CHECK(!long_press(begin, returned));
    }
    {
        using namespace pico_page::platform;
        using namespace pico_page::input;
        ReleasedTap taps;
        CHECK(!taps.accept({EventKind::K3, EventPhase::Begin, 1}, 1));
        CHECK(!taps.accept({EventKind::InputReset, EventPhase::Cancel, 9}, 1));
        CHECK(!taps.accept({EventKind::K3, EventPhase::End, 1}, 1));
        CHECK(!taps.accept({EventKind::K2, EventPhase::Begin, 10}, 1));
        CHECK(taps.accept({EventKind::K2, EventPhase::End, 10}, 1));
    }
    {
        using namespace pico_page::platform;
        using namespace pico_page::input;
        ReleasedTap taps;
        CHECK(!taps.accept({EventKind::Touch, EventPhase::Begin, 20, 300, 520}, 10));
        CHECK(!taps.accept({EventKind::InputReset}, 10));
        CHECK(!taps.accept({EventKind::Touch, EventPhase::End, 20, 300, 520}, 11));
        CHECK(!taps.accept({EventKind::Touch, EventPhase::Begin, 21, 300, 520}, 11));
        CHECK(taps.accept({EventKind::Touch, EventPhase::End, 21, 300, 520}, 11));
        CHECK(!taps.accept({EventKind::Touch, EventPhase::End, 21, 300, 520}, 11));
    }
    {
        using namespace pico_page::platform;
        using namespace pico_page::input;
        TouchMoves moves;
        Event notification{EventKind::Touch, EventPhase::Move, 21, 10, 20, 100};
        CHECK(moves.publish(notification));
        for (int x = 11; x < 1000; ++x)
            CHECK(!moves.publish({EventKind::Touch, EventPhase::Move, 21, x, 20, x}));
        CHECK(!moves.publish({EventKind::Touch, EventPhase::Move, 22, 42, 20, 1100}));
        CHECK(!moves.pending_for(22));
        moves.receive(notification);
        CHECK(notification.sequence == 21 && notification.x == 999);
        CHECK(moves.publish({EventKind::Touch, EventPhase::Move, 22, 42, 20, 1100}));
        CHECK(moves.pending_for(22));
        moves.clear();
        CHECK(moves.publish({EventKind::Touch, EventPhase::Move, 23, 50, 20, 1200}));
        ReleasedTap taps;
        taps.accept({EventKind::Touch, EventPhase::Begin, 1, 100, 100, 1000}, 1);
        taps.accept({EventKind::Touch, EventPhase::Move, 1, 110, 100, 2000}, 1);
        CHECK(taps.pending() && taps.due(1000 + kLongPressUs, 1));
        taps.accept({EventKind::Touch, EventPhase::Move, 1, 140, 100, 3000}, 1);
        CHECK(taps.pending() && !taps.due(1000 + kLongPressUs, 1));
        const auto released = taps.accept({EventKind::Touch, EventPhase::End, 1, 140, 100, 4000}, 1);
        CHECK(released && released->x == 100 && !taps.pending());
        InputRouter owner;
        CHECK(owner.accept({1, Phase::Begin, Input::TouchCenter}));
        CHECK(!owner.accept({1, Phase::Move, Input::TouchCenter}));
        CHECK(!owner.accept({2, Phase::Begin, Input::K3}));
        CHECK(!owner.accept({2, Phase::End, Input::K3}));
        CHECK(!owner.accept({1, Phase::Cancel, Input::TouchCenter}));
        CHECK(owner.accept({3, Phase::Begin, Input::K2}));
    }
    InputRouter input;
    CHECK(input.route(Context::Reading, {1, Phase::Begin, Input::TouchCenter}) == Action::OpenTools);
    CHECK(input.route(Context::Tools, {1, Phase::End, Input::TouchCenter}) == Action::None);
    CHECK(input.route(Context::Tools, {2, Phase::Begin, Input::K2}) == Action::ForceRefresh);
    CHECK(input.route(Context::Reading, {2, Phase::End, Input::K2}) == Action::None);
    CHECK(input.route(Context::Reading, {2, Phase::Begin, Input::K2}) == Action::None);
    CHECK(input.route(Context::Sleeping, {3, Phase::Begin, Input::K2}, true) == Action::Wake);
    CHECK(input.route(Context::Reading, {3, Phase::End, Input::K2}) == Action::None);
    CHECK(input.route(Context::Sleeping, {4, Phase::Begin, Input::K2}) == Action::None);
    CHECK(input.route(Context::Sleeping, {4, Phase::End, Input::K2}) == Action::None);
    CHECK(map_input(Context::Typography, Input::K2) == Action::ForceRefresh);
    CHECK(map_input(Context::Conflict, Input::K2) == Action::ForceRefresh);
    CHECK(map_input(Context::Confirmation, Input::K2) == Action::ForceRefresh);
    CHECK(map_input(Context::LibraryRoot, Input::K2) == Action::ForceRefresh);
    CHECK(map_input(Context::Transfer, Input::K2) == Action::ForceRefresh);
    for (auto context : {Context::Tools, Context::Typography, Context::Conflict, Context::Confirmation}) {
        CHECK(map_input(context, Input::K1) == Action::None);
        CHECK(map_input(context, Input::K3) == Action::None);
    }

    Recorder backend;
    Display display(backend);
    Session session(display);
    const Anchor first{"chapter1.xhtml", "p1", 0};
    const Anchor second{"chapter1.xhtml", "p1", 80};
    session.open("book-a", "source-v1", "device-a");
    auto token = session.request_page(first);
    CHECK(session.layout_ready(token));
    CHECK(!session.visible_record());
    CHECK(!session.request_save());
    CHECK(backend.frames.back().refresh == RefreshIntent::FullClean);
    auto result = display.finish(token, true);
    CHECK(result && session.presented(*result));
    CHECK(session.visible_record()->anchor == first);
    CHECK(session.visible_record()->local_revision == 1);
    auto save = session.request_save();
    CHECK(save && !session.request_save());
    CHECK(session.saved(*save, true));
    CHECK(!session.needs_save());

    CHECK(map_input(Context::Reading, Input::K2) == Action::ForceRefresh);
    CHECK(session.force_refresh());
    const auto refresh = backend.frames.back();
    CHECK(refresh.purpose == FramePurpose::ManualRefresh);
    CHECK(refresh.refresh == RefreshIntent::FullClean);
    CHECK(!session.force_refresh());
    CHECK(backend.frames.size() == 2);
    result = display.finish(refresh.token, true);
    CHECK(result && !session.presented(*result));
    CHECK(session.visible_record()->local_revision == 1 && !session.needs_save());
    CHECK(session.visible_record()->anchor == first);

    token = session.request_page(second);
    CHECK(session.layout_ready(token));
    auto wrong = token;
    ++wrong.request;
    CHECK(!display.finish(wrong, true) && display.busy());
    result = display.finish(token, false);
    CHECK(result && !session.presented(*result));
    CHECK(!display.baseline_valid());
    CHECK(session.visible_record()->anchor == first && !session.needs_save());
    token = session.request_page(second);
    CHECK(session.layout_ready(token));
    CHECK(backend.frames.back().refresh == RefreshIntent::FullClean);
    result = display.finish(token, true);
    CHECK(result && session.presented(*result));
    CHECK(!session.presented(*result));
    CHECK(session.visible_record()->local_revision == 2 && session.needs_save());

    save = session.request_save();
    token = session.request_page(first, RefreshIntent::Content, true, PageTurnOrigin::Top);
    CHECK(session.layout_ready(token));
    CHECK(backend.frames.back().reverse_page_effect && backend.frames.back().page_turn_origin == PageTurnOrigin::Top);
    result = display.finish(token, true);
    CHECK(session.presented(*result));
    CHECK(session.saved(*save, true));
    CHECK(session.saved_record()->local_revision == 2 && session.needs_save());
    save = session.request_save();
    CHECK(!session.saved(*save, false));
    CHECK(session.needs_save());
    save = session.request_save();
    CHECK(save);
    CHECK(session.saved(*save, true));

    auto old_layout = session.request_page(second);
    session.change_layout();
    CHECK(!session.layout_ready(old_layout));
    token = session.request_page(first);
    CHECK(session.layout_ready(token));
    session.change_layout();
    result = display.finish(token, true);
    CHECK(!session.presented(*result));
    CHECK(session.visible_record()->local_revision == 3);
    token = session.request_page(second);
    CHECK(session.layout_ready(token));
    session.open("book-b", "source-v1", "device-a");
    result = display.finish(token, true);
    CHECK(!session.presented(*result));
    CHECK(!session.visible_record() && !session.request_save());
    session.open("book-a", "source-v1", "device-a");
    CHECK(!session.presented(*result));
    token = session.request_page(first);
    auto newest = session.request_page(second);
    CHECK(!session.layout_ready(token));
    backend.reject = true;
    CHECK(!session.layout_ready(newest));
    CHECK(!display.busy() && !display.baseline_valid() && !session.visible_record());
    backend.reject = false;
    CHECK(session.layout_ready(newest));
    result = display.finish(newest, true);
    CHECK(session.presented(*result));

    auto overlay = Frame{OperationContext{"book-a", 99, 99, 99}, second, FramePurpose::Overlay};
    CHECK(display.present(overlay));
    result = display.finish(overlay.token, true);
    CHECK(!session.presented(*result));
    CHECK(session.visible_record()->local_revision == 1);

    auto old_save = session.request_save();
    CHECK(old_save);
    session.open("book-a", "source-v1", "device-a");
    token = session.request_page(second);
    CHECK(session.layout_ready(token));
    result = display.finish(token, true);
    CHECK(session.presented(*result));
    save = session.request_save();
    CHECK(save && save->record == old_save->record);
    CHECK(!session.saved(*old_save, true) && session.needs_save());
    CHECK(session.saved(*save, true));

    token = session.request_page(first);
    CHECK(session.layout_ready(token));
    newest = session.request_page(second);
    result = display.finish(token, true);
    CHECK(!session.presented(*result));
    CHECK(session.visible_record()->anchor == second);
    CHECK(session.layout_ready(newest));
    result = display.finish(newest, true);
    CHECK(session.presented(*result));
    auto resume_record = *session.visible_record();
    session.open("book-a", "source-v1", "device-a", resume_record);
    token = session.request_page(resume_record.anchor);
    CHECK(session.layout_ready(token));
    result = display.finish(token, true);
    CHECK(session.presented(*result));
    CHECK(*session.visible_record() == resume_record && !session.needs_save());
    std::cout << "PASS: input ownership, manual refresh, presentation and save generations (host only)\n";
}
