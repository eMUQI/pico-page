---
name: verify-pico-page
description: Verify Pico Page changes with its existing host tests, command-line tools, headless reader, and authorized device workbench. Use after an implementation or fix, and for reproduction or acceptance work in this repository. This skill does not authorize flashing, deployment, or hardware operations.
---

# Verify Pico Page

Read the root AGENTS.md verification matrix and select the affected paths from [the feature map](features/README.md). Reuse those commands and existing assertions. Report a missing prerequisite instead of silently reducing coverage.

## Launch

Run from the repository root. For Python tool changes, run the actual CLI with the same input before and after the change, plus its existing unittest target. For shared C++ changes, follow `docs/development/environment.md` to prepare the pinned fonts and toolchain, configure and build host, and inspect `ctest --test-dir build/host -N` before running the required tests. Use `tools/host/README.md` for the headless reader. Build firmware when AGENTS requires it.

For a device path, read `docs/development/diagnostic-workbench.md` first. Use an already identified diagnostic device only with current task authorization. This skill does not flash or select a partition. Do not open a serial port already owned by another process.

## Doctor

Check `git status --short` and the current commit before a run. For a CLI, run its `--help` and verify the input files exist and contain the intended synthetic or authorized data. For host, confirm the requested test is registered and its fonts and fixtures are available. For a device, verify the attached build, test conditions, serial ownership, and initial state through the existing console and workbench checks. A responding process or accepted input alone does not prove the requested behavior.

## Drive

Follow the selected feature entry. Exercise the normal command or input route and inspect its output and side effects. Avoid internal state setters as a substitute for the actual user path. Use a private test card for host and synthetic books for automated device scenarios. Keep one writer per card directory and one owner per serial device.

## Evidence

Store original inputs, commands, exit status, stdout, stderr, and resulting artifacts under a new directory in `.local/verification/`. Do not commit credentials, private books, raw device logs, or complete sdkconfig files. For a bug, retain the failing-before and passing-after results from the same reproduction. When the CLI reports a verdict in JSON, inspect that verdict as well as its process exit code.

Separate source checks, host behavior, sanitizer results, firmware builds, device protocol evidence, and physical acceptance. Headless pixels do not establish panel quality; a fixture or model does not establish TF reliability. Summarize failures and missing evidence instead of converting them into success.

## Cleanup

Stop only processes started by this run and close their file or serial handles. Preserve proof artifacts after cleanup. Restore a test device's temporary settings only within the task's authorization and record any state left behind. Do not erase user files or discard unrelated checkout changes. Finish with the applicable checks from AGENTS, `python3 tools/check_handoff.py`, and `git diff --check`.
