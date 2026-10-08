# JPEGDEC progressive thumbnail decoder

Source: [bitbank2/JPEGDEC](https://github.com/bitbank2/JPEGDEC/tree/86282979224c8a32fd51e091ed5a35b0c699a52b),
commit `86282979224c8a32fd51e091ed5a35b0c699a52b`. Only `src/JPEGDEC.cpp`,
`src/JPEGDEC.h`, `src/jpeg.inl` and the Apache-2.0 `LICENSE` are included.
There are no Arduino, display-helper, assembler or esp-dsp dependencies.

Patches 0001–0003 come from [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader/tree/43694c9018d26446ab30cce2fa65ee608355ae50/scripts/jpegdec_patches),
commit `43694c9018d26446ab30cce2fa65ee608355ae50`. They correct skipped-MCU pointer/DC writes
and absent chroma components in progressive scans. Original patch authorship is retained;
`CrossPoint-MIT.txt` preserves the source repository's license. The decoder retains Apache-2.0.

Pico Page patch 0004 disables ARM SIMD when `NO_SIMD` is selected, assembles endian values
without unaligned typed reads, preserves 32/64-bit bit-reader widths, and guards short Huffman
code shifts and negative coefficient shifts. It also marks modified source files.
Apply the four patches in order to the pinned upstream tree; then copy the three source files.
The parent `manifest.json` records original and integrated hashes.

The adapter in `image.cpp` uses this decoder only after TJpgDec reports an unsupported JPEG
variant, and accepts only progressive mode. Baseline JPEG remains on TJpgDec. Progressive
output uses DC coefficients at 1/8 width and height, then the existing grayscale rasterizer.
This supports cover thumbnails, not full-detail progressive image reconstruction. Read, seek
and MCU draw callbacks check cancellation. Decoder state is heap-owned by the calling job;
no global file, new task, full-resolution RGB plane or full-image coefficient array is used.

Redistribution must include `LICENSE`, applicable copyright/attribution notices and modification
notices; retain `CrossPoint-MIT.txt` with the borrowed patches. These may accompany firmware in
release documentation; no device UI attribution is required by these licenses. This component
record does not replace the project's separate BSP/LGPL and other dependency obligations.
