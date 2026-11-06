#pragma once

#include "pico_page/core.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace pico_page::platform {

struct DisplayInfo { int width = 0, height = 0; };
/** Available before initialize(), without powering the display or allocating buffers. */
DisplayInfo display_info();

/**
 * Presents a frame in the configured portrait geometry synchronously. Packed grayscale uses
 * even x in the low nibble, odd x in the high nibble, 0 black and 15 white.
 * The caller owns packed and keeps it immutable until this function returns.
 * full_clean always drives physical clearing, including for unchanged pixels.
 * With a valid baseline, an identical frame may complete successfully without
 * powering or scanning the panel, including when the caller's anchor changes.
 * region limits gray updates in portrait coordinates when the baseline is valid.
 * changed_pixels_only holds equal nonwhite pixels and retains the waveform's
 * white-to-white maintenance during gray updates; clearing
 * and underrun recovery always ignore this mask and rebuild the full baseline.
 * hold_unchanged_footer holds equal nonwhite pixels in the display profile's reading-footer region during
 * body updates, including directional effects; white-to-white retains the source
 * waveform maintenance drive. Clearing and recovery ignore it.
 * Recovery may clear and redraw the whole frame. Early argument or owner rejection
 * preserves the previous baseline. A failure after validation invalidates it.
 */
bool present(const std::uint8_t* packed, std::size_t size, bool full_clean,
             std::string& error, std::optional<RefreshRegion> region = std::nullopt,
             bool changed_pixels_only = false, bool hold_unchanged_footer = false);

/**
 * Nonblocking display admission on the application task. A false result defers
 * presentation and reserves priority over new network receive steps. Retry while
 * the frame is pending, or cancel_display() when it is discarded. A successful
 * admission must be paired with finish_display(), including preparation failure.
 */
bool try_begin_display();
void finish_display();
/** Withdraws a deferred request; call only when no display admission is held. */
void cancel_display();

/** Content-update profiles; full-clean frames always use the provider's clearing profile. */
enum class PageWaveform {
    Gray16,
    Gray8,
    Ripple,
};

void set_page_waveform(PageWaveform waveform, bool reverse_page_effect = false);

/**
 * Selects a volatile diagnostic override on the application task between presents.
 * Gray16 disables the override; Ripple enables the effect.
 * Failure leaves the previous selection intact. Does not refresh or save settings.
 */
bool set_page_effect_waveform(PageWaveform waveform, std::string& error);
/** Application-task query; Gray16 means use the configured refresh profile. */
PageWaveform page_effect_waveform();

struct PresentTiming {
    std::int64_t copy_us = 0;
    std::int64_t power_us = 0;
    std::int64_t draw_us = 0;   // Physical update, including any clearing.
};
PresentTiming present_timing();

}
