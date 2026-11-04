# Verify device interaction

## Sub-features

Use the existing `refresh`, `reading-flow`, and `image-sync` modes when their paths are affected. Keep manual observations separate from automatic verdicts.

## How to get to it (user POV)

The user reads a synthetic book, triggers K2 force refresh, changes typography, or checks synchronization through normal application actions.

## Driving it with device_workbench

Follow `docs/development/diagnostic-workbench.md` to identify the diagnostic firmware, private conditions JSON, free serial port, and synthetic card-relative book path. Run `python3 tools/device_workbench.py --help`, then the documented scenario command with the observed paths and a new output directory. Do not invent port names, build provenance, network results, or device authorization. Inspect the resulting `run.json`, raw operation trace, and relevant canvas. Confirm the scenario verdict and required physical observations separately.

## Gotchas

These scenarios can save progress and change test-book settings. One process owns the serial port. Incomplete traces, missing conditions, or unavailable required network service are not passes. Unavailable hardware leaves this feature unverified; do not replace it with a mock and report device success.
