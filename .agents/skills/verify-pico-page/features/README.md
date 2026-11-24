# Pico Page verification paths

Choose the entries touched by the task. Detailed behavior and acceptance remain in the repository documents linked below.

| Feature | Verification entry | Existing authority |
| --- | --- | --- |
| Diagnostic report comparison | [Report comparison](reports.md) | `docs/development/device-diagnostics.md` |
| Open, turn, and render a book | [Host reading](reading.md) | `tools/host/README.md`, `tests/README.md` |
| Collect and view local reading time | [Reading statistics tests and host captures](../../../../tests/README.md#本机阅读统计) | [Timing, recovery and device acceptance](../../../../docs/development/acceptance.md#本机阅读统计) |
| Force refresh and reading on hardware | [Device interaction](device.md) | `docs/development/device-diagnostics.md`, `docs/development/acceptance.md` |

Only paths actually exercised in a run are verified. The device entry requires authorized hardware and is not covered by a host-only run.
