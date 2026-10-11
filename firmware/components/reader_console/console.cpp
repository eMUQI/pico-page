#include "pico_page/console.h"
#include "pico_page/diagnostics.h"
#include "sdkconfig.h"

#ifdef CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE
#include "pico_page/platform.h"
#include "store_internal.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace pico_page::console {
namespace {
constexpr std::size_t line_limit = 4224;
constexpr std::size_t screen_limit = 256;
TaskHandle_t owner = nullptr;
std::array<char, line_limit> line{};
std::array<char, 512> received{};
std::size_t received_offset = 0, received_size = 0;
std::size_t used = 0;
bool discard = false;
std::uint64_t frame_number = 0;
bool frame_success = false;
FramePurpose frame_purpose = FramePurpose::Overlay;
RefreshIntent frame_refresh = RefreshIntent::Content;
std::unique_ptr<FileImport> transfer;
std::int64_t transfer_activity = 0;
std::uint32_t reply_failures = 0;
std::int64_t reply_deadline_us = 0;

// A complete metrics or trace response shares one 20 ms transport budget.
struct ExportBudget {
    ExportBudget() { reply_deadline_us = esp_timer_get_time() + 20000; }
    ~ExportBudget() { reply_deadline_us = 0; }
};

void send_line(TickType_t wait, const char *format, std::va_list arguments) {
    // Whole protocol lines must not use the log VFS's best-effort, per-byte transmitter.
    static std::array<char, 1536> output;
    output[0] = '\n';
    const int count = std::vsnprintf(output.data() + 1, output.size() - 1, format, arguments);
    if (count < 0 || static_cast<std::size_t>(count) >= output.size() - 1 ||
        usb_serial_jtag_write_bytes(output.data(), static_cast<std::size_t>(count) + 1, wait) != count + 1)
        ++reply_failures;
}

void reply(const char *format, ...) __attribute__((format(printf, 1, 2)));
void reply(const char *format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    const auto remaining_ms = reply_deadline_us
        ? std::max<std::int64_t>(0, reply_deadline_us - esp_timer_get_time()) / 1000 : 1000;
    // IDF may wait once for the TX mutex and once for ring space.
    send_line(pdMS_TO_TICKS(reply_deadline_us ? remaining_ms / 2 : remaining_ms), format, arguments);
    va_end(arguments);
}

void notify(const char *format, ...) __attribute__((format(printf, 1, 2)));
void notify(const char *format, ...) {
    // Unsolicited diagnostics must not stall input or progress saving when no host reads USB.
    std::va_list arguments;
    va_start(arguments, format);
    send_line(0, format, arguments);
    va_end(arguments);
}

bool token(std::string_view &input, std::string_view &value) {
    const auto first = input.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        input = {};
        value = {};
        return false;
    }
    input.remove_prefix(first);
    const auto end = input.find(' ');
    value = input.substr(0, end);
    input = end == std::string_view::npos ? std::string_view{} : input.substr(end + 1);
    return true;
}

template <typename T> bool number(std::string_view input, T &value, int base = 10) {
    if (input.empty())
        return false;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value, base);
    return parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size();
}

void error(std::uint32_t id, const char *reason) {
    reply("PP %lu ERROR %s\n", static_cast<unsigned long>(id), reason);
}

const char *view_name(View view) {
    switch (view) {
    case View::PageEffects: return "page_effects";
    case View::TextEntry: return "text_entry";
    case View::Loading: return "loading";
    case View::NetworkClosing: return "network_closing";
    case View::Library:
        return "library";
    case View::Reading:
        return "reading";
    case View::Tools:
        return "tools";
    case View::Contents:
        return "contents";
    case View::Typography:
        return "typography";
    case View::Refresh:
        return "refresh";
    case View::Settings:
        return "settings";
    case View::UsbTransfer:
        return "usb_transfer";
    case View::Import:
        return "import";
    case View::Sync:
        return "sync";
    case View::Error:
        return "error";
    case View::Sleeping:
        return "sleeping";
    case View::RefreshConfirm: return "refresh_confirm";
    case View::RefreshInterval: return "refresh_interval";
    case View::BookDetails: return "book_details";
    case View::WifiSettings: return "wifi_settings";
    case View::Help: return "help";
    case View::PowerSettings: return "power_settings";
    case View::LockScreens: return "lock_screens";
    case View::PowerMenu: return "power_menu";
    case View::PowerOff: return "power_off";
    case View::StorageSettings: return "storage_settings";
    case View::InputSettings: return "input_settings";
    case View::BookManagement: return "book_management";
    case View::SyncSettings: return "sync_settings";
    case View::Portal: return "portal";
    case View::Confirmation:
        return "confirmation";
    }
    return "unknown";
}

std::string hex(const std::uint8_t *bytes, std::size_t count) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output(count * 2, '0');
    for (std::size_t i = 0; i < count; ++i) {
        output[2 * i] = digits[bytes[i] >> 4];
        output[2 * i + 1] = digits[bytes[i] & 15];
    }
    return output;
}

void status(std::uint32_t id, const ReaderApp &app, bool card_available) {
    const auto &pixels = app.canvas().pixels();
    const auto battery = platform::battery();
    const auto crc = esp_rom_crc32_le(0, pixels.data(), pixels.size());
    const auto revision = app.saved_record() ? app.saved_record()->local_revision : 0;
    // Identity of the saved source position without printing the resource path or any text.
    std::uint32_t anchor = 0;
    if (const auto &record = app.saved_record()) {
        const auto &position = record->anchor;
        anchor = esp_rom_crc32_le(0, reinterpret_cast<const std::uint8_t *>(position.resource.data()), position.resource.size());
        anchor = esp_rom_crc32_le(anchor, reinterpret_cast<const std::uint8_t *>(position.node.data()), position.node.size());
        anchor = esp_rom_crc32_le(anchor, reinterpret_cast<const std::uint8_t *>(&position.offset), sizeof(position.offset));
    }
    const auto pages = app.book_pages();
    const auto caches = app.cache_usage();
    const auto input = platform::input_diagnostics();
    reply("PP %lu OK view=%s pending=%d card=%d saved=%d revision=%llu anchor=%08lx font_px=%d page=%lu pages=%lu count=%s sync=%s sync_order=%d "
          "content_busy=%d save_busy=%d save_due=%d largest_psram=%u frame=%llu frame_ok=%d "
          "purpose=%d clean=%d bytes=%u crc32=%08lx heap=%u min_heap=%u internal=%u "
          "largest_internal=%u "
          "psram=%u stack_bytes=%u input_stack_bytes=%u input_overflows=%u battery_valid=%d battery_pct=%d importing=%d received=%llu "
          "tx_failures=%lu uptime_ms=%llu copy_ms=%u power_ms=%u draw_ms=%u "
          "cache_total=%u cache_target=%u cache_evictions=%llu cache_body=%u cache_next=%u cache_bitmap=%u bitmap_hits=%llu cache_pages=%u cache_covers=%u "
          "glyph_ui_bytes=%u glyph_body_bytes=%u glyph_ui=%u glyph_body=%u advances_ui=%u advances_body=%u\n",
          static_cast<unsigned long>(id), view_name(app.view()), !!app.pending_frame(),
          card_available, !!app.saved_record(), static_cast<unsigned long long>(revision),
          static_cast<unsigned long>(anchor), app.font_size_px(),
          static_cast<unsigned long>(pages ? pages->page : 0),
          static_cast<unsigned long>(pages ? pages->total : 0), app.background_state(), app.sync_state(),
          app.sync_order(),
          app.content_busy(), app.save_busy(), app.save_due(),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
          static_cast<unsigned long long>(frame_number), frame_success,
          static_cast<int>(frame_purpose), frame_refresh == RefreshIntent::FullClean,
          static_cast<unsigned>(pixels.size()), static_cast<unsigned long>(crc),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(
              heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
          static_cast<unsigned>(input.stack_free_bytes), static_cast<unsigned>(input.overflows), battery.valid,
          battery.percent, !!transfer,
          static_cast<unsigned long long>(transfer ? transfer->received() : 0),
          static_cast<unsigned long>(reply_failures),
          static_cast<unsigned long long>(esp_timer_get_time() / 1000),
          static_cast<unsigned>(platform::present_timing().copy_us / 1000),
          static_cast<unsigned>(platform::present_timing().power_us / 1000),
          static_cast<unsigned>(platform::present_timing().draw_us / 1000),
          static_cast<unsigned>(caches.retained), static_cast<unsigned>(caches.target),
          static_cast<unsigned long long>(caches.evictions), static_cast<unsigned>(caches.body_bitmap), static_cast<unsigned>(caches.next_layout),
          static_cast<unsigned>(caches.next_bitmap), static_cast<unsigned long long>(caches.bitmap_hits),
          static_cast<unsigned>(caches.page_index), static_cast<unsigned>(caches.cover_pixels),
          static_cast<unsigned>(caches.ui_glyph_bytes), static_cast<unsigned>(caches.body_glyph_bytes),
          static_cast<unsigned>(caches.ui_glyphs), static_cast<unsigned>(caches.body_glyphs),
          static_cast<unsigned>(caches.ui_advances), static_cast<unsigned>(caches.body_advances));
}

void list(std::uint32_t id, const char *root, std::string_view arguments) {
    if (!root) {
        error(id, "no_card");
        return;
    }
    std::string_view offset_text;
    std::uint32_t offset = 0;
    if (!token(arguments, offset_text) || !number(offset_text, offset) || offset > 4096) {
        error(id, "usage_ls_offset_relative_path");
        return;
    }
    const std::string relative(arguments);
    if (relative != "." &&
        (relative.size() > 512 || store_internal::safe_path(root, relative) != StoreStatus::Ok)) {
        error(id, "unsafe_or_missing_path");
        return;
    }
    const auto directory = relative == "." ? std::string(root) : std::string(root) + "/" + relative;
    DIR *entries = opendir(directory.c_str());
    if (!entries) {
        error(id, "directory_open_failed");
        return;
    }
    std::uint32_t index = 0, emitted = 0;
    bool more = false;
    while (const auto *entry = readdir(entries)) {
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, ".."))
            continue;
        if (index++ < offset)
            continue;
        if (emitted == 32) {
            more = true;
            break;
        }
        const auto path = directory + "/" + entry->d_name;
        struct stat info{};
        if (stat(path.c_str(), &info) != 0)
            continue;
        const auto name =
            hex(reinterpret_cast<const std::uint8_t *>(entry->d_name), std::strlen(entry->d_name));
        reply("PP %lu ENTRY kind=%s bytes=%llu name_hex=%s\n", static_cast<unsigned long>(id),
              S_ISDIR(info.st_mode) ? "dir" : "file", static_cast<unsigned long long>(info.st_size),
              name.c_str());
        ++emitted;
    }
    closedir(entries);
    reply("PP %lu OK entries=%lu next=%lu more=%d\n", static_cast<unsigned long>(id),
          static_cast<unsigned long>(emitted), static_cast<unsigned long>(more ? index - 1 : index),
          more);
}

#ifdef CONFIG_PICO_PAGE_PERF_METRICS
void metrics(std::uint32_t id) {
    ExportBudget budget;
    const auto snapshot = diagnostics::snapshot();
    for (unsigned i = 0; i < snapshot.metrics.size(); ++i) {
        const auto& m = snapshot.metrics[i];
        reply("PP %lu ENTRY kind=%u started=%llu active=%llu success=%llu failure=%llu cancelled=%llu timeout=%llu total_us=%llu max_us=%llu\n",
              static_cast<unsigned long>(id), i, m.started, m.active, m.outcomes[0], m.outcomes[1],
              m.outcomes[2], m.outcomes[3], m.total_us, m.max_us);
    }
    reply("PP %lu OK schema=1 queued=%u dropped=%llu sequence=%llu tx_failures=%lu stopped=%d internal_min=%u psram_min=%u\n",
          static_cast<unsigned long>(id), static_cast<unsigned>(snapshot.queued), snapshot.dropped, snapshot.sequence,
          static_cast<unsigned long>(reply_failures), snapshot.stopped,
          static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));
}
#endif
#ifdef CONFIG_PICO_PAGE_TRACE_EVENTS
void trace(std::uint32_t id) {
    ExportBudget budget;
    diagnostics::Event event;
    // Bounded response; export runs on the console owner, outside recorder critical sections.
    for (unsigned i = 0; i < 8 && diagnostics::pop(event); ++i) {
        reply("PP %lu ENTRY seq=%llu time_us=%llu op=%llu duration_us=%llu task=%lu kind=%u begin=%d result=%u "
              "session=%llu layout=%llu frame=%llu request=%llu detail=%lu\n",
              static_cast<unsigned long>(id), event.sequence, event.time_us, event.operation, event.duration_us,
              static_cast<unsigned long>(event.task), static_cast<unsigned>(event.kind), event.begin,
              static_cast<unsigned>(event.outcome), event.context.session, event.context.layout,
              event.context.frame, event.context.request, static_cast<unsigned long>(event.context.detail));
    }
    const auto snapshot = diagnostics::snapshot();
    reply("PP %lu OK schema=1 queued=%u dropped=%llu sequence=%llu tx_failures=%lu stopped=%d\n", static_cast<unsigned long>(id),
          static_cast<unsigned>(snapshot.queued), snapshot.dropped, snapshot.sequence, static_cast<unsigned long>(reply_failures), snapshot.stopped);
}
#endif

void execute(ReaderApp &app, const char *root, std::uint64_t &sequence, bool actions_allowed, bool input_allowed) {
    std::string_view input(line.data(), used), word;
    std::uint32_t id = 0;
    if (!token(input, word) || word != "PP" || !token(input, word) || !number(word, id) || !id ||
        !token(input, word)) {
        error(id, "usage_PP_id_command");
        return;
    }
#ifdef CONFIG_PICO_PAGE_PERF_METRICS
    if (word == "metrics" && input.empty()) { metrics(id); return; }
#endif
#ifdef CONFIG_PICO_PAGE_TRACE_EVENTS
    if (word == "trace" && input.empty()) { trace(id); return; }
    if (word == "trace_start" && input.empty()) {
        diagnostics::start(); metrics(id); return;
    }
    if (word == "trace_stop" && input.empty()) {
        diagnostics::stop(); metrics(id); return;
    }
#endif
    if (word == "status" && input.empty()) {
        status(id, app, root != nullptr);
        return;
    }
    if (word == "ls") {
        list(id, root, input);
        return;
    }
    if (word == "abort" && input.empty()) {
        transfer.reset();
        reply("PP %lu OK aborted=1\n", static_cast<unsigned long>(id));
        return;
    }
    if (word == "data" || word == "commit") {
        if (!transfer) {
            error(id, "no_import");
            return;
        }
        std::string detail;
        if (word == "commit") {
            if (!input.empty()) {
                transfer.reset();
                error(id, "invalid_commit");
                return;
            }
            const auto result = transfer->finish(detail);
            transfer.reset();
            if (result != StoreStatus::Ok) {
                error(id, detail.c_str());
                return;
            }
            reply("PP %lu OK committed=1\n", static_cast<unsigned long>(id));
            return;
        }
        std::string_view offset_text, data, extra;
        std::uint64_t offset = 0;
        if (!token(input, offset_text) || !token(input, data) || token(input, extra) ||
            !number(offset_text, offset) || data.empty() || data.size() > 4096 || data.size() % 2) {
            transfer.reset();
            error(id, "invalid_import_block");
            return;
        }
        std::array<std::uint8_t, 2048> decoded{};
        for (std::size_t i = 0; i < data.size() / 2; ++i) {
            unsigned value = 0;
            if (!number(data.substr(2 * i, 2), value, 16)) {
                transfer.reset();
                error(id, "invalid_import_hex");
                return;
            }
            decoded[i] = static_cast<std::uint8_t>(value);
        }
        if (transfer->append(offset, decoded.data(), data.size() / 2, detail) != StoreStatus::Ok) {
            transfer.reset();
            error(id, detail.c_str());
            return;
        }
        transfer_activity = esp_timer_get_time();
        reply("PP %lu OK received=%llu\n", static_cast<unsigned long>(id),
              static_cast<unsigned long long>(transfer->received()));
        return;
    }
    if (word == "begin") {
        if (!actions_allowed || app.pending_frame() || transfer) {
            error(id, "busy");
            return;
        }
        if (!root) {
            error(id, "no_card");
            return;
        }
        std::string_view size_text, crc_text;
        std::uint64_t size = 0;
        std::uint32_t crc = 0;
        if (!token(input, size_text) || !token(input, crc_text) || !number(size_text, size) ||
            crc_text.size() != 8 || !number(crc_text, crc, 16)) {
            error(id, "usage_begin_size_crc32_path");
            return;
        }
        std::string detail;
        app.invalidate_identity_cache();
        transfer = FileStore(root).begin_import(std::string(input), size, crc, detail);
        if (!transfer) {
            error(id, detail.c_str());
            return;
        }
        transfer_activity = esp_timer_get_time();
        reply("PP %lu OK ready=1 block=2048\n", static_cast<unsigned long>(id));
        return;
    }
    if (word == "file") {
        if (transfer || !actions_allowed) {
            error(id, "busy");
            return;
        }
        if (!root) {
            error(id, "no_card");
            return;
        }
        std::string detail;
        std::uint64_t size = 0;
        std::uint32_t crc = 0;
        if (FileStore(root).inspect_asset(std::string(input), size, crc, detail) !=
            StoreStatus::Ok) {
            error(id, detail.c_str());
            return;
        }
        reply("PP %lu OK bytes=%llu crc32=%08lx\n", static_cast<unsigned long>(id),
              static_cast<unsigned long long>(size), static_cast<unsigned long>(crc));
        return;
    }
    if (word == "screen") {
        std::string_view frame_text, offset_text, count_text, extra;
        std::uint64_t expected_frame = 0;
        std::size_t offset = 0, count = 0;
        if (!token(input, frame_text) || !token(input, offset_text) || !token(input, count_text) ||
            token(input, extra) || !number(frame_text, expected_frame) ||
            !number(offset_text, offset) || !number(count_text, count) || !count ||
            count > screen_limit) {
            error(id, "usage_screen_frame_offset_count_max256");
            return;
        }
        const auto &pixels = app.canvas().pixels();
        if (!frame_success || !frame_number || app.pending_frame() ||
            expected_frame != frame_number) {
            error(id, "frame_not_confirmed_or_changed");
            return;
        }
        if (offset > pixels.size() || count > pixels.size() - offset) {
            error(id, "screen_range");
            return;
        }
        const auto data = hex(pixels.data() + offset, count);
        reply("PP %lu OK frame=%llu offset=%u count=%u hex=%s\n", static_cast<unsigned long>(id),
              static_cast<unsigned long long>(frame_number), static_cast<unsigned>(offset),
              static_cast<unsigned>(count), data.c_str());
        return;
    }
    const bool input_command = word == "key" || word == "tap" || word == "hold" || word == "swipe";
    if ((!actions_allowed && !(input_allowed && input_command)) || app.pending_frame() || transfer) {
        error(id, "busy");
        return;
    }
    if (word == "waveform") {
        PageEffect effect;
        if (input == "default") effect = PageEffect::Off;
        else if (input == "A") effect = PageEffect::TextTurnA;
        else if (input == "B") effect = PageEffect::TextTurnB;
        else if (input == "C") effect = PageEffect::TextTurnC;
        else if (input == "ripple") effect = PageEffect::Ripple;
        else { error(id, "usage_waveform_default_A_B_C_ripple"); return; }
        app.set_page_effect(effect);
        reply("PP %lu OK waveform=%.*s\n", static_cast<unsigned long>(id),
              static_cast<int>(input.size()), input.data());
        return;
    }
    if (word == "open") {
        const std::string path(input);
        if (!root || path.size() > 512 ||
            store_internal::safe_path(root, path) != StoreStatus::Ok) {
            error(id, "unsafe_or_missing_path");
            return;
        }
        if (!app.open_book(path)) {
            error(id, "open_failed");
            return;
        }
    } else if (word == "key" || word == "tap" || word == "hold" || word == "swipe") {
        AppInput event{{0, Phase::Begin, Input::TouchCenter}, word == "tap" || word == "hold", 0, 0, false, word == "hold"};
        std::string_view first, second, extra;
        if (!token(input, first)) {
            error(id, "missing_input");
            return;
        }
        if (event.touch) {
            if (!token(input, second) || token(input, extra) || !number(first, event.x) ||
                !number(second, event.y) || event.x < 0 || event.x >= platform::width ||
                event.y < 0 || event.y >= platform::height) {
                error(id, "invalid_touch");
                return;
            }
        } else if (word == "swipe") {
            if (!input.empty() || (first != "left" && first != "right")) {
                error(id, "invalid_swipe");
                return;
            }
            event.event.input = first == "left" ? Input::Next : Input::Previous;
        } else {
            if (!input.empty() || (first != "K1" && first != "K2" && first != "K3")) {
                error(id, "invalid_key");
                return;
            }
            event.event.input = first == "K1" ? Input::K1 : first == "K2" ? Input::K2 : Input::K3;
        }
        if (sequence == std::numeric_limits<std::uint64_t>::max()) {
            error(id, "sequence_exhausted");
            return;
        }
        event.event.sequence = ++sequence;
        app.input(event);
        event.event.phase = Phase::End;
        app.input(event);
    } else {
        error(id, "unknown_command");
        return;
    }
    reply("PP %lu OK accepted pending=%d\n", static_cast<unsigned long>(id), !!app.pending_frame());
}
} // namespace

bool initialize(std::string &error_message) {
    error_message.clear();
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        config.rx_buffer_size = 8192;
        config.tx_buffer_size = 2048;
        if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
            error_message = "Cannot install diagnostic USB driver";
            return false;
        }
    }
    usb_serial_jtag_vfs_use_driver();
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        error_message = "Cannot configure diagnostic stdin";
        return false;
    }
    owner = xTaskGetCurrentTaskHandle();
    notify("PP 0 READY version=1 max_line=%u max_screen=%u\n", static_cast<unsigned>(line_limit),
          static_cast<unsigned>(screen_limit));
    return true;
}

bool shutdown(std::string &error) {
    error.clear();
    if (transfer) { error = "USB import is active"; return false; }
    owner = nullptr;
    usb_serial_jtag_vfs_use_nonblocking();
    if (usb_serial_jtag_is_driver_installed()) usb_serial_jtag_driver_uninstall();
    used = received_offset = received_size = 0;
    return true;
}

bool poll(ReaderApp &app, const char *card_root, std::uint64_t &input_sequence,
          bool actions_allowed, bool input_allowed) {
    if (!owner || owner != xTaskGetCurrentTaskHandle())
        return false;
    if (transfer && (!card_root || esp_timer_get_time() - transfer_activity > 30'000'000)) {
        const auto count = transfer->received();
        transfer.reset();
        reply("PP 0 IMPORT_ABORT reason=%s received=%llu partial=%u\n",
              card_root ? "timeout" : "card_unavailable", static_cast<unsigned long long>(count),
              static_cast<unsigned>(used));
    }
    for (unsigned i = 0; i < line_limit + 1; ++i) {
        if (received_offset == received_size) {
            const auto count = usb_serial_jtag_read_bytes(received.data(), received.size(), 0);
            if (count <= 0)
                return false;
            received_offset = 0;
            received_size = static_cast<std::size_t>(count);
        }
        const char ch = received[received_offset++];
        if (ch == '\r')
            continue;
        if (ch == '\n') {
            if (discard) {
                transfer.reset();
                error(0, "invalid_or_oversized_line");
            }
            bool activity = false;
            if (!discard && used) {
                std::string_view request(line.data(), used), word;
                token(request, word); // PP
                token(request, word); // request ID
                token(request, word); // command
                activity = word != "status" && word != "metrics" && word != "trace" && word != "trace_start" && word != "trace_stop" &&
                           word != "screen" && word != "ls" && word != "file";
                execute(app, card_root, input_sequence, actions_allowed, input_allowed);
            }
            used = 0;
            discard = false;
            return activity;
        }
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f || used == line.size())
            discard = true;
        if (!discard)
            line[used++] = ch;
    }
    return false;
}

bool transfer_active() {
    return !!transfer;
}

void frame_completed(const Frame &frame, bool success) {
    if (!owner || owner != xTaskGetCurrentTaskHandle())
        return;
    ++frame_number;
    frame_success = success;
    frame_purpose = frame.purpose;
    frame_refresh = frame.refresh;
    notify("PP 0 FRAME number=%llu success=%d purpose=%d clean=%d\n",
          static_cast<unsigned long long>(frame_number), success, static_cast<int>(frame.purpose),
          frame.refresh == RefreshIntent::FullClean);
}
} // namespace pico_page::console
#else
namespace pico_page::console {
bool initialize(std::string &error) {
    error.clear();
    return true;
}
bool shutdown(std::string &error) { error.clear(); return true; }
bool poll(ReaderApp &, const char *, std::uint64_t &, bool, bool) {
    return false;
}
void frame_completed(const Frame &, bool) {}
bool transfer_active() {
    return false;
}
} // namespace pico_page::console
#endif
