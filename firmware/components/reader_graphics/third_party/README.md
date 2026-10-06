# Image decoder sources

Sources and licenses were inspected before copying on 2026-10-06. The decoder wrappers
and `tjpgdcnf.h` are local integration code. `manifest.json` records upstream and local hashes.

| Component | Exact source | License and retained notice |
|---|---|---|
| pngle | `kikuchan/pngle` commit `b1c68193f1d3f8642b3e0e095d457a828038e6fb`, `src/pngle.c`, `src/pngle.h` | MIT; original headers and `pngle/LICENSE` |
| TJpgDec R0.03 | `MindReset/read_pico_firmware` commit `28cde682a4468a581c278761922724f57d976418`, `main/book/vendor/tjpgd.c`, `tjpgd.h` | ChaN's permissive notice in `tjpgd.c` permits use/modification/redistribution and requires keeping the source notice; the source repository's Apache-2.0 license is also retained for its changes |
| miniz | Existing reviewed `reader_document/third_party/miniz` | Existing MIT/public-domain notices; linked once, no second inflate implementation |

Upstream source locations:
- https://github.com/kikuchan/pngle/tree/b1c68193f1d3f8642b3e0e095d457a828038e6fb
- https://github.com/MindReset/read_pico_firmware/tree/28cde682a4468a581c278761922724f57d976418/main/book/vendor

The Espressif documentation search confirmed TJpgDec's MCU callback and fixed-workspace
approach for ESP32-S3. The shared C source is used on both host and device, so host checks
exercise the decoder actually shipped; the ROM implementation is not substituted.
Reference: https://components.espressif.com/components/espressif/esp_jpeg

## Local pngle changes

- Cast the high byte to `uint32_t` before a 24-bit shift; avoid signed-shift undefined behavior.
- Clamp empty Adam7 pass width before subtraction; prevent an underflow allocation for narrow images.
- Ignore the optional `bKGD` suggestion while retaining CRC checking. The reader composites alpha
  on white. The upstream branch iterated six RGB chunk bytes into a four-element temporary array.
- Require deflate completion before IEND, and limit inflated bytes using the declared dimensions.
- Keep the temporary RGBA tuple per decoder instead of in a static shared array.

Gamma correction is disabled. PNG CRC, palette and filter checks remain active. The wrapper
requires a valid terminal IEND and the exact decoded pixel count. Bounds are checked before
any dimension-dependent decoder allocation.

TJpgDec `.c` and `.h` are unchanged. The local configuration uses RGB888 output, 512-byte input,
scaling enabled, `JD_FASTDECODE=1`, no clipping table and no default camera Huffman tables.
The decoder receives an 8 KiB heap workspace; it never allocates a full RGB image.
