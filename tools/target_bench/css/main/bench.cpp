#include "css.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <new>

using namespace pico_page::document::css;
namespace {
constexpr uint32_t content_stack_bytes = 32768;
constexpr unsigned repetitions = 5;
struct BenchElement { const char* tag; const char* id; const char* classes; const char* inline_style; int parent; };
struct BenchInput { std::string_view css; const BenchElement* elements; size_t count; };
#ifdef CSS_BENCH_DATA
#include CSS_BENCH_DATA
#else
constexpr BenchElement elements[] = {{"body", "", "", "", -1}, {"section", "", "chapter", "", 0},
    {"p", "opening", "body opening", "", 1}, {"span", "", "emphasis", "font-weight:bold", 2}};
constexpr BenchInput bench_inputs[] = {{
    "p{font-size:1.2em;line-height:150%;margin:1em 2%;text-indent:2em}"
    ".chapter > p{font-weight:bold} #opening{font-size:14pt} p .emphasis{font-style:italic}",
    elements, sizeof(elements) / sizeof(elements[0])}};
#endif
constexpr uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
struct Measurement {
    Phase phase = Phase::Complete;
    int64_t phase_start = 0, last_check = 0, last_yield = 0, max_gap_us = 0;
    int64_t parse_us = 0, match_us = 0, cascade_us = 0;
    size_t minimum_largest_internal = SIZE_MAX, minimum_largest_psram = SIZE_MAX;
    void check_interval(int64_t now) {
        if (last_check) max_gap_us = std::max(max_gap_us, now - last_check);
        last_check = now;
    }
    void sample_resources(int64_t now) {
        minimum_largest_internal = std::min(minimum_largest_internal, heap_caps_get_largest_free_block(internal_caps));
        minimum_largest_psram = std::min(minimum_largest_psram, heap_caps_get_largest_free_block(psram_caps));
        if (now - last_yield >= 2000) { vTaskDelay(1); last_yield = esp_timer_get_time(); }
    }
    static bool checkpoint(void* opaque) {
        auto& self = *static_cast<Measurement*>(opaque);
        const auto now = esp_timer_get_time();
        self.check_interval(now);
        self.sample_resources(now);
        return false;
    }
    static void changed(void* opaque, Phase next) {
        auto& self = *static_cast<Measurement*>(opaque);
        const auto now = esp_timer_get_time();
        const auto elapsed = self.phase_start ? now - self.phase_start : 0;
        if (self.phase == Phase::Parse) self.parse_us += elapsed;
        if (self.phase == Phase::Match) self.match_us += elapsed;
        if (self.phase == Phase::Cascade) self.cascade_us += elapsed;
        self.sample_resources(now);
        self.phase = next;
        self.phase_start = esp_timer_get_time();
    }
};
void metric(const char* stage, int64_t us) {
    std::printf("reader_perf: stage=%s us=%lld min_us=0\n", stage, static_cast<long long>(us));
}
void run(void*) {
    std::printf("CSS_BENCH cpu_mhz=240 stack_bytes=%lu priority=1 repetitions=%u\n",
                static_cast<unsigned long>(content_stack_bytes), repetitions);
    for (size_t sample = 0; sample < sizeof(bench_inputs) / sizeof(bench_inputs[0]); ++sample) {
        const auto& input = bench_inputs[sample];
        if (input.count > 4096) { std::printf("CSS_BENCH invalid_input\n"); continue; }
        auto nodes = std::unique_ptr<Element[]>(new(std::nothrow) Element[input.count]);
        auto styles = std::unique_ptr<ComputedStyle[]>(new(std::nothrow) ComputedStyle[input.count]);
        if (!nodes || !styles) { std::printf("CSS_BENCH allocation_failed\n"); continue; }
        for (size_t i = 0; i < input.count; ++i) {
            const auto& node = input.elements[i];
            if (node.parent >= static_cast<int>(i) || node.parent < -1) { std::printf("CSS_BENCH invalid_parent\n"); vTaskDelete(nullptr); }
            nodes[i] = {node.tag, node.id, node.classes, node.parent < 0 ? nullptr : &nodes[node.parent]};
        }
        for (unsigned repeat = 0; repeat < repetitions; ++repeat) {
            if (heap_caps_monitor_local_minimum_free_size_start() != ESP_OK) {
                std::printf("CSS_BENCH heap_monitor_failed\n"); vTaskDelete(nullptr);
            }
            Measurement measurement;
            measurement.sample_resources(esp_timer_get_time());
            const auto internal_before = heap_caps_get_free_size(internal_caps);
            const auto psram_before = heap_caps_get_free_size(psram_caps);
            const Checkpoint checkpoint{Measurement::checkpoint, &measurement, Measurement::changed};
            const auto allocation_start = esp_timer_get_time();
            measurement.check_interval(allocation_start);
            StyleSheet sheet;
            const auto allocation_us = esp_timer_get_time() - allocation_start;
            const auto parsed = sheet.append(input.css, checkpoint);
            WorkBudget budget;
            Result computed = Result::Complete;
            for (size_t i = 0; i < input.count; ++i) {
                const auto parent = input.elements[i].parent;
                computed = sheet.compute(nodes[i], parent < 0 ? nullptr : &styles[parent], {}, input.elements[i].inline_style, budget, styles[i], checkpoint);
                if (computed != Result::Complete) break;
            }
            measurement.check_interval(esp_timer_get_time());
            const auto internal_low = heap_caps_get_minimum_free_size(internal_caps);
            const auto psram_low = heap_caps_get_minimum_free_size(psram_caps);
            heap_caps_monitor_local_minimum_free_size_stop();
            std::printf("CSS_BENCH sample=%u repeat=%u bytes=%u elements=%u parse_result=%u compute_result=%u work=%lu allocated_bytes=%u internal_before_bytes=%u psram_before_bytes=%u internal_low_bytes=%u psram_low_bytes=%u internal_largest_low_bytes=%u psram_largest_low_bytes=%u stack_low_bytes=%u\n",
                static_cast<unsigned>(sample), repeat, static_cast<unsigned>(input.css.size()), static_cast<unsigned>(input.count),
                static_cast<unsigned>(parsed), static_cast<unsigned>(computed), static_cast<unsigned long>(budget.work),
                static_cast<unsigned>(sheet.report().allocated_bytes), static_cast<unsigned>(internal_before),
                static_cast<unsigned>(psram_before), static_cast<unsigned>(internal_low), static_cast<unsigned>(psram_low),
                static_cast<unsigned>(measurement.minimum_largest_internal), static_cast<unsigned>(measurement.minimum_largest_psram),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            metric("css_allocate", allocation_us); metric("css_parse", measurement.parse_us);
            metric("css_match", measurement.match_us); metric("css_cascade", measurement.cascade_us);
            metric("css_checkpoint_max", measurement.max_gap_us);
            vTaskDelay(1);
        }
    }
    std::printf("CSS_BENCH complete\n");
    vTaskDelete(nullptr);
}
} // namespace
extern "C" void app_main() {
    // Allow the host to reopen the console after the application reset.
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (xTaskCreate(run, "css_bench", content_stack_bytes, nullptr, 1, nullptr) != pdPASS)
        std::printf("CSS_BENCH task_create_failed\n");
}
