# Phosphor Icons

- Source: https://github.com/phosphor-icons/core
- Commit: `2b75f3ad12b420c9504ef05df8d2564a28f8500e`
- License: MIT; retain `LICENSE` with source and binary distribution notices.
- Files: seven unmodified regular SVG icons for dictionary navigation, close, and resize.
- `dictionary_icons.h` contains their pre-rasterized 4-bit alpha masks; no SVG parser or icon package runs in the firmware.
- No runtime dependencies.

## Rasterization

SVG assets are rasterized offline with Sharp 0.35.5 / librsvg 2.63.2 into
40 × 40 RGBA pixels without altering their square viewBox. Alpha is rounded to
4 bits (`round(alpha / 17)`); each byte stores the even pixel in the low nibble
and the odd pixel in the high nibble, in row-major order. The seven masks occupy
5,600 bytes of read-only firmware data. The application composites them in the
normal or disabled gray onto the existing white panel, without runtime scaling,
SVG decoding, heap allocation, or file access.

Header mapping: `PreviousSource` = rewind, `NextSource` = fast-forward,
`Close` = x, `PreviousPage` = caret-left, `NextPage` = caret-right,
`Expand` = arrows-out-simple, `Collapse` = arrows-in-simple.

Regenerate with `node rasterize.cjs` from this directory in an environment
providing the Sharp version above. Sharp and librsvg are maintenance tools only;
normal host and firmware builds consume the checked-in header. `manifest.json`
records the SHA-256 of each original SVG.
