/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once

#include <stdint.h>
#include <string.h>

/**
 * Build a phase-independent drive mask before LCD scanning starts. Input contains
 * one (to, from) nibble pair per pixel. The compact output begins at the preceding
 * 64-pixel boundary so its offset and length are aligned for the S3 vector mask.
 * Each pixel uses two bits, first pixel in bits 1:0; equal nonwhite suffix pixels
 * become zero. White-to-white retains the source waveform's maintenance drive;
 * a waveform with no white maintenance remains all-hold. Other bits remain one.
 * Buffers must be disjoint and cover width
 * input bytes and the returned output length. Width must be divisible by 64.
 * Invalid geometry returns zero without writing output. No allocation occurs.
 */
static inline int epd_build_unchanged_suffix_mask(const uint8_t* difference, uint8_t* mask,
                                                 int first_column, int width) {
    if (first_column < 0 || first_column >= width || width % 64 != 0) return 0;
    const int first_pixel = first_column / 64 * 64;
    const int bytes = (width - first_pixel) / 4;
    memset(mask, 0xFF, bytes);
    for (int x = first_column; x < width; ++x) {
        const uint8_t pixel = difference[x];
        if (pixel != 0xFF && (pixel & 15u) == (pixel >> 4))
            mask[(x - first_pixel) / 4] &= (uint8_t)~(3u << (2 * (x % 4)));
    }
    return bytes;
}
