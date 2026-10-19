/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once

#include <stdint.h>

/**
 * Suppress equal grayscale transitions in a native-column suffix after S3 VE
 * LUT conversion. Input contains one (to, from) nibble pair per pixel; output
 * contains four two-bit drive actions per byte, first pixel in bits 1:0.
 * Buffers must cover width input bytes and width / 4 output bytes and must be
 * disjoint; width must be divisible by four. A negative first_column or invalid
 * width disables masking. No allocation occurs.
 */
static inline void epd_hold_unchanged_suffix(const uint8_t* difference, uint8_t* output,
                                             int first_column, int width) {
    if (first_column < 0 || first_column >= width || width % 4 != 0) return;
    for (int x = first_column; x < width; ++x) {
        const uint8_t pixel = difference[x];
        if ((pixel & 15u) == (pixel >> 4))
            output[x / 4] &= (uint8_t)~(3u << (2 * (x % 4)));
    }
}
