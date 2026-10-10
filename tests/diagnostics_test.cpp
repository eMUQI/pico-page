#include "pico_page/diagnostics.h"
#include <cassert>
#include <thread>
#include <vector>
using namespace pico_page::diagnostics;
int main() {
    int evaluated = 0;
    PP_DIAGNOSTIC(++evaluated;)
#if defined(CONFIG_PICO_PAGE_PERF_METRICS) && CONFIG_PICO_PAGE_PERF_METRICS
    assert(evaluated == 1);
    Recorder recorder;
    const Context context{11, 22, 33, 44, 55};
    const auto first = recorder.begin(Kind::Frame, context, 100, 7);
    for (unsigned i = 0; i < 4; ++i) {
        auto token = recorder.begin(Kind::Content, {}, 200, 8);
        recorder.end(token, static_cast<Outcome>(i), 200 + i * 10, 8);
    }
    recorder.end(first, Outcome::Failure, 500, 7);
    auto state = recorder.snapshot();
    assert(state.metrics[0].active == 0 && state.metrics[0].outcomes[1] == 1);
    assert(state.metrics[0].max_us == 400 && state.metrics[0].total_us == 400);
    for (auto count : state.metrics[3].outcomes) assert(count == 1);
    Event event;
#ifdef CONFIG_PICO_PAGE_TRACE_EVENTS
    assert(recorder.pop(event) && event.begin && event.context.layout == 22 && event.task == 7);
    assert(event.operation == first.id && event.sequence == 1);
    for (unsigned i = 0; i < 70; ++i) recorder.begin(Kind::Input, {}, 600 + i, 7);
    state = recorder.snapshot();
    assert(state.queued == 64 && state.dropped == 15 && state.sequence == 80);
    std::uint64_t sequence = 1;
    while (recorder.pop(event)) assert(event.sequence == ++sequence);
    auto token = recorder.begin(Kind::Network, {}, 900, 8);
    recorder.end(token, Outcome::Timeout, 950, 8);
    assert(recorder.pop(event) && event.sequence == 81);
#else
    assert(!recorder.pop(event) && state.queued == 0 && state.sequence == 0);
    static_assert(sizeof(Recorder) < 512, "Performance must not allocate trace storage");
#endif
    // Unfinished operations remain visible, independent of ring overflow.
#ifdef CONFIG_PICO_PAGE_TRACE_EVENTS
    assert(recorder.snapshot().metrics[1].active == 70);
#endif
#ifdef CONFIG_PICO_PAGE_TRACE_EVENTS
    recorder.start();
    const auto unfinished = recorder.begin(Kind::Content, {}, 1000, 8);
    recorder.stop();
    const auto frozen = recorder.snapshot();
    assert(frozen.stopped && frozen.metrics[3].active == 1);
    const auto ignored = recorder.begin(Kind::Frame, {}, 1010, 7);
    recorder.end(unfinished, Outcome::Success, 1020, 8);
    assert(ignored.id == 0 && recorder.snapshot().sequence == frozen.sequence);
    recorder.start();
    recorder.end(unfinished, Outcome::Success, 1030, 8);
    recorder.end(ignored, Outcome::Success, 1030, 8);
    assert(recorder.snapshot().sequence == 0 && recorder.snapshot().metrics[3].active == 0);
    const auto fresh = recorder.begin(Kind::Frame, {}, 1040, 7);
    recorder.end(fresh, Outcome::Success, 1050, 7);
    recorder.stop();
    assert(recorder.pop(event) && event.operation == fresh.id);
    assert(recorder.pop(event) && !event.begin && event.sequence == 2);
    assert(!recorder.pop(event));
#endif
    std::vector<std::thread> writers;
    for (unsigned i = 0; i < 4; ++i) writers.emplace_back([] {
        for (unsigned j = 0; j < 1000; ++j) {
            auto token = begin(Kind::Frame);
            end(token, Outcome::Success);
        }
    });
    for (auto& writer : writers) writer.join();
    assert(snapshot().metrics[0].outcomes[0] == 4000);
    assert(snapshot().metrics[0].active == 0);
#else
    assert(evaluated == 0);
    PP_DIAGNOSTIC(undefined_function_must_not_be_evaluated();)
#endif
}
