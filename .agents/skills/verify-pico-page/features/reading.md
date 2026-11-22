# Open and render a book on host

## Sub-features

Open a synthetic book, turn pages, force refresh, inspect captured pixels, and confirm saved position through the existing behavior tests.

## How to get to it (user POV)

The reader opens a book from the library and uses the same application actions as the device. Host substitutes the display backend.

## Driving it with the headless reader

Use the build and dependency commands in `tools/host/README.md`. Prepare a dedicated card directory with a generated EPUB from `build/host/document-fixtures`, the pinned test font, and test settings. Run `build/host/pico_page --card` with that directory plus `--headless --script` with a local script path. Use the documented `open`, `k3`, `k2`, and `capture` commands. Give `capture` an absolute output path. Inspect the captured image and compare persisted position where the task requires it. Run the affected registered CTest entries from `tests/README.md`; use the full matrix for shared core changes.

## Gotchas

Do not point automated input at a personal library. Missing fonts or fixtures are prerequisites to fix, not grounds to skip assertions. A captured bitmap verifies host rendering and cannot establish refresh waveform, physical touch, ghosting, or power behavior.
